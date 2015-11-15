#include "gadgetron_rest.h"

namespace Gadgetron
{
  ReST* Gadgetron::ReST::instance_ = nullptr;
  unsigned int Gadgetron::ReST::port_ = 9080;
  
  ReST* ReST::instance()
  {
    if (!instance_) {
      instance_ = new ReST();
      instance_->app_.route_dynamic("/")([]() {
	  return "<html><body><h1>GADGETRON</h1></body></html>\n";
	});

	instance_->open();
      }
      return instance_;
    }

    void ReST::open()
    {
      crow::logger::setLogLevel(crow::LogLevel::ERROR);
      Gadgetron::ReST* tmp = this;
      server_thread_ = std::thread([tmp](){
	  tmp->app_.port(port_)
	  .multithreaded()
	  .run();
	});
    }
}
