/*
 *  Copyright 2001-2007 Internet2
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file shibsp/remoting/RemotedHandler.h
 * 
 * Base class for handlers that need SP request/response layer to be remoted. 
 */

#ifndef __shibsp_remhandler_h__
#define __shibsp_remhandler_h__

#include <shibsp/SPRequest.h>
#include <shibsp/Handler.h>
#include <shibsp/remoting/ListenerService.h>

namespace shibsp {

    /**
     * Base class for handlers that need HTTP request/response layer to be remoted.
     */
    class SHIBSP_API RemotedHandler : public Handler, public Remoted 
    {
    public:
        virtual ~RemotedHandler() {}

    protected:
        RemotedHandler(const xercesc::DOMElement* e=NULL) {}

        /**
         * Wraps a request by annotating an outgoing data flow with the data needed
         * to remote the request information.
         *
         * @param request   an SPRequest to remote
         * @param in        the dataflow object to annotate
         * @param headers   array of request headers to copy to remote request
         * @param certs     true iff client certificates should be available for the remote request
         * @return  the input dataflow object
         */
        const DDF& wrap(const SPRequest& request, DDF& in, const std::vector<std::string>& headers, bool certs=false) const;
        
        /**
         * Unwraps a response by examining an incoming data flow to determine
         * whether a response was produced by the remoted handler. 
         * 
         * @param request   SP request context
         * @param out       the dataflow object to unpack
         * @return  a pair containing a "request completed" indicator and a server-specific response code
         */
        virtual std::pair<bool,long> unwrap(SPRequest& request, DDF& out) const;
        
        /**
         * Builds a new request instance around a remoted data object.
         * 
         * @param in    the dataflow object containing the remoted request
         * @return  a call-specific request object based on the input, to be freed by the caller 
         */
        opensaml::HTTPRequest* getRequest(DDF& in) const;
        
        /**
         * Builds a new response instance around an outgoing data object.
         * 
         * @param out   the dataflow object to be returned by the caller
         * @return  a call-specific response object, to be freed by the caller 
         */
        opensaml::HTTPResponse* getResponse(DDF& out) const;
    };
};

#endif /* __shibsp_remhandler_h__ */
