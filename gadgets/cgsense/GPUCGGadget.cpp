#include "GPUCGGadget.h"
#include "Gadgetron.h"
#include "GadgetMRIHeaders.h"
#include "GadgetXml.h"
#include "ndarray_vector_td_utilities.h"

#include "tinyxml.h"

GPUCGGadget::GPUCGGadget()
	: slice_no_(0)
	, profiles_per_frame_(48)
	, shared_profiles_(16)
	, channels_(0)
	, samples_per_profile_(0)
	, device_number_(0)
	, number_of_iterations_(5)
	, oversampling_(1.25)
	, kernel_width_(5.5)
	, kappa_(0.1)
	, is_configured_(false)
	, current_profile_offset_(0)
	, current_frame_number_(0)
	, allocated_samples_(0)
	, data_host_ptr_(0)
	, kspace_acc_coil_images_os_size_(0)
	, csm_buffer_length_(8)
{
	matrix_size_    = uintd2(0,0);
	matrix_size_os_ = uintd2(0,0);
	pass_on_undesired_data_ = true; // We will make one of these for each slice and so data should be passed on.
}

GPUCGGadget::~GPUCGGadget() {}

int GPUCGGadget::process_config( ACE_Message_Block* mb )
{
	GADGET_DEBUG1("GPUCGGadget::process_config\n");

	slice_no_ = get_int_value(std::string("sliceno").c_str());
	pass_on_undesired_data_ = get_bool_value(std::string("pass_on_undesired_data").c_str());

	TiXmlDocument doc;
	doc.Parse(mb->rd_ptr());

	if (!is_configured_) {

		samples_per_profile_ = GetIntParameterValueFromXML(&doc, "encoding","readout_length");
		channels_ = GetIntParameterValueFromXML(&doc,"encoding","channels");

		if (matrix_size_.vec[0] == 0 && matrix_size_.vec[1] == 0) {
			matrix_size_ = uintd2(GetIntParameterValueFromXML(&doc,"encoding","matrix_x"), GetIntParameterValueFromXML(&doc,"encoding","matrix_y"));
		}

		GADGET_DEBUG2("Matrix size  : [%d,%d] \n", matrix_size_.vec[0], matrix_size_.vec[1]);

		matrix_size_os_ = 
			uintd2(static_cast<unsigned int>(ceil((matrix_size_.vec[0]*oversampling_)/32.0f)*32),  //TODO: use warp_size from deviceProp over 32 - it might change some day
			       static_cast<unsigned int>(ceil((matrix_size_.vec[1]*oversampling_)/32.0f)*32)); //TODO: use warp_size from deviceProp over 32 - it might change some day

		GADGET_DEBUG2("Matrix size OS: [%d,%d] \n", matrix_size_os_.vec[0], matrix_size_os_.vec[1]);

//		if (image_dev_ptr_) cudaFree(image_dev_ptr_);
//		cudaMalloc( (void**) &image_dev_ptr_, prod(matrix_size_)*sizeof(cuFloatComplex) );

		cudaError_t err = cudaGetLastError();
		if( err != cudaSuccess ){
			GADGET_DEBUG2("Failed to allocate memory for image: %s\n", cudaGetErrorString(err));
			return GADGET_FAIL;
		}

		// do we really need this plan?!?
		plan_.setup( matrix_size_, matrix_size_os_, kernel_width_, device_number_ );

		// Allocate encoding operator for non-Cartesian Sense
		E_ = boost::shared_ptr< cgOperatorNonCartesianSense<float,2> >( new cgOperatorNonCartesianSense<float,2>() );  

		// Allocate preconditioner
		D_ = boost::shared_ptr< cuCGPrecondWeights<float_complext::Type> >( new cuCGPrecondWeights<float_complext::Type>() );

		// Allocate regularization image operator and corresponding rhs operator
		rhs_buffer_ = boost::shared_ptr< cgOperatorSenseRHSBuffer<float,2> >( new cgOperatorSenseRHSBuffer<float,2>() );
		R_ = boost::shared_ptr< cuCGImageOperator<float,float_complext::Type> >( new cuCGImageOperator<float,float_complext::Type>() );  
		R_->set_weight( kappa_ );
		R_->set_encoding_operator( rhs_buffer_ );

		// Setup solver
		cg_.add_matrix_operator( E_ );  // encoding matrix
//		cg_.add_matrix_operator( R_ );  // regularization matrix
		cg_.set_preconditioner ( D_ );  // preconditioning matrix
		cg_.set_iterations( number_of_iterations_ );
		cg_.set_limit( 1e-6 ); // TODO: take this from configuration
		cg_.set_output_mode( cuCGSolver<float, float_complext::Type>::OUTPUT_VERBOSE ); // TODO: once it is all working, change to silent output

		// We do not have a csm yet, so initialize a dummy one to purely ones
		csm_ = boost::shared_ptr< cuNDArray<float_complext::Type> >( new cuNDArray<float_complext::Type> );
		std::vector<unsigned int> csm_dims = uintd_to_vector<2>(matrix_size_); csm_dims.push_back( channels_ );
		csm_->create( &csm_dims );
		cuNDA_clear<float_complext::Type>( csm_.get(), get_one<float_complext::Type>() );

		// Setup matrix operator
		E_->set_csm(csm_);
		E_->setup( matrix_size_, matrix_size_os_, kernel_width_ );
		is_configured_ = true;
	}

	return 0;
}


int  GPUCGGadget::process(GadgetContainerMessage<GadgetMessageAcquisition>* m1,
	GadgetContainerMessage< hoNDArray< std::complex<float> > >* m2)
{
	if (!is_configured_) {
		GADGET_DEBUG1("Data received before configuration complete\n");
		return GADGET_FAIL;
	}

	//Is this data for me?
	if (m1->getObjectPtr()->idx.slice != slice_no_) {

		//This data is not for me
		if (pass_on_undesired_data_) {
			this->next()->putq(m1);
		} else {
			GADGET_DEBUG2("Dropping slice: %d\n", m1->getObjectPtr()->idx.slice);
			m1->release();
		}
		return GADGET_OK;
	}

	buffer_.enqueue_tail(m1);

	// TODO: why don't we gather all profiles in one prior gadget and pass it here when we are ready to reconstruct?

	if ((int)buffer_.message_count() >= profiles_per_frame_) {

		if (calculate_trajectory() == GADGET_FAIL) {
			GADGET_DEBUG1("Failed to calculate the trajectory on the GPU\n");
			return GADGET_FAIL;
		}

		if (calculate_density_compensation() == GADGET_FAIL) { // TODO: we don't actually need to recompute the dcw every frame
			GADGET_DEBUG1("Failed to calculate the density compensation on the GPU\n");
			return GADGET_FAIL;
		}

		if (upload_samples() == GADGET_FAIL) {
			GADGET_DEBUG1("Failed to upload samples to the GPU\n");
			return GADGET_FAIL;
		}

		//    int samples_in_frame = profiles_per_frame_*samples_per_profile_;
		E_->set_dcw(dcw_);

		if( E_->preprocess(traj_.get()) < 0 ) {
			GADGET_DEBUG1("Error during cgOperatorNonCartesianSense::preprocess()\n");
			return GADGET_FAIL;
		}

		// Define preconditioning weights
		boost::shared_ptr< cuNDArray<float> > _precon_weights = cuNDA_ss<float,float_complext::Type>( csm_.get(), 2 );
//		cuNDA_axpy<float>( kappa_, R_->get(), _precon_weights.get() );  
		cuNDA_reciprocal_sqrt<float>( _precon_weights.get() );
		boost::shared_ptr< cuNDArray<float_complext::Type> > precon_weights = cuNDA_real_to_complext<float>( _precon_weights.get() );
		_precon_weights.reset();
		D_->set_weights( precon_weights );
		precon_weights.reset();

		// TODO: no noise decorrelation in new design yet...
		/*
		float shutter_radius = 0.85*0.5;
		if (!noise_decorrelate_generic(samples_in_frame, 
		channels_, 
		shutter_radius, 
		data_dev_ptr_, 
		trajectory_dev_ptr_ )) {

		GADGET_DEBUG1("Error during noise decorrelation\n");
		return GADGET_FAIL;
		} 
		*/   

		/*
		if (m_csm_needs_reset) {
		AllocateCSMBuffer();
		}
		*/

		// TODO:
		// No csm buffer update strategy in new design yet...
		/*
		float sigma_csm = 16.0f;
		unsigned long ptr_offset = 
		(current_frame_number_%csm_buffer_length_)*prod(matrix_size_os_)*channels_;

		if (!update_csm_and_regularization( plan_generic_, 
		data_dev_ptr_,
		trajectory_dev_ptr_, 
		dcw_dev_ptr_,
		sigma_csm, 
		&csm_buffer_dev_ptr_[ptr_offset],
		csm_acc_coil_image_os_dev_ptr_,
		kspace_acc_coil_images_os_size_, 
		true,
		false )) { //csm_needs_reset

		GADGET_DEBUG1("update_csm_and_regularization failed\n");
		return GADGET_FAIL;    
		}
		*/

		// Form rhs
		std::vector<unsigned int> rhs_dims = uintd_to_vector<2>(matrix_size_);
		cuNDArray<float_complext::Type> rhs; rhs.create(&rhs_dims);
		E_->mult_MH( device_samples_.get(), &rhs );

		boost::shared_ptr< cuNDArray<float_complext::Type> > cgresult = cg_.solve(&rhs);

		if (!cgresult.get()) {
			GADGET_DEBUG1("iterative_sense_compute failed\n");
			return GADGET_FAIL;
		}

		//Now pass the reconstructed image on
		GadgetContainerMessage<GadgetMessageImage>* cm1 = 
			new GadgetContainerMessage<GadgetMessageImage>();

		GadgetContainerMessage< hoNDArray< std::complex<float> > >* cm2 = 
			new GadgetContainerMessage< hoNDArray< std::complex<float> > >();

		cm1->cont(cm2);

		std::vector<unsigned int> img_dims(2);
		img_dims[0] = matrix_size_.vec[0];
		img_dims[1] = matrix_size_.vec[1];

		// TODO: Should we pass cgresult->to_host() downstream instead? What happens to shared_ptr if we pass it down? 
		// Is it rexreated before this method terminates and otherwise deletes it?

		if (!cm2->getObjectPtr()->create(&img_dims)) {
			GADGET_DEBUG1("Unable to allocate new image array");
			cm1->release();
			return GADGET_FAIL;
		}

		size_t data_length = prod(matrix_size_);

		// TODO: use cgresult->to_host (see above)
/*		cudaMemcpy(cm2->getObjectPtr()->get_data_ptr(),
			image_dev_ptr_,
			data_length*sizeof(cuFloatComplex),
			cudaMemcpyDeviceToHost);
			*/
		cm1->getObjectPtr()->matrix_size[0] = img_dims[0];
		cm1->getObjectPtr()->matrix_size[1] = img_dims[1];
		cm1->getObjectPtr()->matrix_size[2] = 1;
		cm1->getObjectPtr()->channels       = 1;
		cm1->getObjectPtr()->data_idx_min       = m1->getObjectPtr()->min_idx;
		cm1->getObjectPtr()->data_idx_max       = m1->getObjectPtr()->max_idx;
		cm1->getObjectPtr()->data_idx_current   = m1->getObjectPtr()->idx;	

		memcpy(cm1->getObjectPtr()->position,m1->getObjectPtr()->position,
			sizeof(float)*3);

		memcpy(cm1->getObjectPtr()->quarternion,m1->getObjectPtr()->quarternion,
			sizeof(float)*4);

		if (this->next()->putq(cm1) < 0) {
			GADGET_DEBUG1("Failed to result image on to Q\n");
			cm1->release();
			return GADGET_FAIL;
		}

		//GADGET_DEBUG2("Frame %d reconstructed an passed down the chain\n",
		//current_frame_number_);

		//Dequeue the message we don't need anymore
		ACE_Message_Block* mb_tmp;
		for (int i = 0; i < (profiles_per_frame_-shared_profiles_); i++) {
			buffer_.dequeue_head(mb_tmp);
			mb_tmp->release();
			current_profile_offset_++; 
		}

		current_frame_number_++;
	}

	return GADGET_OK;
}


int GPUCGGadget::copy_samples_for_profile(float* host_base_ptr,
	std::complex<float>* data_base_ptr,
	int profile_no,
	int channel_no)
{

	// TODO: this should be achieved using array copies?

	memcpy(host_base_ptr + 
		(channel_no*allocated_samples_ + profile_no*samples_per_profile_) * 2,
		data_base_ptr + channel_no*samples_per_profile_, 
		sizeof(float)*samples_per_profile_*2);

	return GADGET_OK;
}

int GPUCGGadget::upload_samples()
{
	// TODO: this should be achieved using array copies?

	int samples_needed = 
		samples_per_profile_*
		profiles_per_frame_;

	if (samples_needed != allocated_samples_) {
//		if (data_dev_ptr_) cudaFree(data_dev_ptr_);

//		cudaMalloc( (void**) &data_dev_ptr_, 
	//		channels_*samples_needed*sizeof(cuFloatComplex) );

		cudaError_t err = cudaGetLastError();
		if( err != cudaSuccess ){
			GADGET_DEBUG2("Unable to allocate GPU memory for samples: %s",
				cudaGetErrorString(err));

			return GADGET_FAIL;
		}

		try {
			data_host_ptr_ = new float[channels_*samples_needed*2];
		} catch (...) {
			GADGET_DEBUG1("Failed to allocate host memory for samples\n");
			return GADGET_FAIL;
		}

		allocated_samples_ = samples_needed;

	}

	ACE_Message_Queue_Reverse_Iterator<ACE_MT_SYNCH> it(buffer_);
	int profiles_copied = 0;
	GadgetContainerMessage<GadgetMessageAcquisition>* m1;
	GadgetContainerMessage< hoNDArray< std::complex<float> > >* m2;
	ACE_Message_Block* mb;

	while (profiles_copied < profiles_per_frame_) {
		it.next(mb);

		m1 = dynamic_cast< GadgetContainerMessage< GadgetMessageAcquisition >* >(mb);
		if (!m1) {
			GADGET_DEBUG1("Failed to dynamic cast message\n");
			return -1;
		}


		m2 = dynamic_cast< GadgetContainerMessage< hoNDArray< std::complex<float> > >* > (m1->cont());

		if (!m2) {
			GADGET_DEBUG1("Failed to dynamic cast message\n");
			return -1;
		}

		// TODO: this is a waste. Copy directly into device array
		std::complex<float>* d = m2->getObjectPtr()->get_data_ptr();
		int current_profile = profiles_per_frame_-profiles_copied-1;

		for (int i = 0; i < channels_; i++) {
			copy_samples_for_profile(data_host_ptr_,
				d,
				current_profile,
				i);
		}


		it.advance();   
		profiles_copied++;
	}
	
	std::vector<unsigned int> dims; dims.push_back(samples_needed); dims.push_back(channels_);
	hoNDArray<float_complext::Type> *tmp = new hoNDArray<float_complext::Type>();
	tmp->create( &dims, (float_complext::Type*)data_host_ptr_, false ); // REMOVE THIS CASTING

	host_samples_ = boost::shared_ptr< hoNDArray<float_complext::Type> >( tmp );
	device_samples_ = boost::shared_ptr< cuNDArray<float_complext::Type> >( new cuNDArray<float_complext::Type>(host_samples_.get()) );
	
/*
	cudaMemcpy( data_dev_ptr_,
		data_host_ptr_,
		samples_needed*channels_*sizeof(cuFloatComplex), 
		cudaMemcpyHostToDevice );
		*/
	cudaError_t err = cudaGetLastError();
	if( err != cudaSuccess ){
		GADGET_DEBUG2("Unable to upload samples to GPU memory: %s",
			cudaGetErrorString(err));
		return GADGET_FAIL;
	}

	//GADGET_DEBUG1("Samples uploaded to GPU\n");

	return GADGET_OK;
}

int GPUCGGadget::allocate_csm_buffer()
{
/*
if (csm_buffer_dev_ptr_) cudaFree(csm_buffer_dev_ptr_);
if (csm_acc_coil_image_os_dev_ptr_) cudaFree(csm_acc_coil_image_os_dev_ptr_);

cudaMalloc( (void**) &csm_buffer_dev_ptr_, 
channels_*prod(matrix_size_os_)*csm_buffer_length_*sizeof(cuFloatComplex) );

cudaError_t err = cudaGetLastError();
if( err != cudaSuccess ){
GADGET_DEBUG2("Unable to allocate CSM buffer: %s \n",
cudaGetErrorString(err));

return GADGET_FAIL;
}

clear_image( prod(matrix_size_os_)*channels_*csm_buffer_length_, 
make_cuFloatComplex(0.0f, 0.0f), 
csm_buffer_dev_ptr_ );

//Allocate memory for accumulated coil images
cudaMalloc( (void**) &csm_acc_coil_image_os_dev_ptr_, 
prod(matrix_size_os_)*channels_*sizeof(cuFloatComplex) );

err = cudaGetLastError();
if( err != cudaSuccess ){
GADGET_DEBUG2("Unable to allocate eccumulated CSM images: %s \n",
cudaGetErrorString(err));

return GADGET_FAIL;
}

clear_image( prod(matrix_size_os_)*channels_, 
make_cuFloatComplex(0.0f, 0.0f), 
csm_acc_coil_image_os_dev_ptr_ );

kspace_acc_coil_images_os_size_ = prod(matrix_size_os_)*channels_;

//m_csm_needs_reset = true;
*/
	return GADGET_OK;
}