/*
 * mod_shire.cpp -- the SHIRE Apache Module
 *
 * Created by:	Derek Atkins <derek@ihtfp.com>
 *
 * $Id$
 */

// SAML Runtime
#include <saml/saml.h>
#include <shib/shib.h>
#include <shib/shib-threads.h>
#include <shib-target/shib-target.h>
#include <xercesc/util/regx/RegularExpression.hpp>

// Apache specific header files
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <http_main.h>
#define CORE_PRIVATE
#include <http_core.h>
#include <http_log.h>

#include <fstream>
#include <sstream>

using namespace std;
using namespace saml;
using namespace shibboleth;
using namespace shibtarget;

extern "C" module MODULE_VAR_EXPORT shire_module;
int shire_handler(request_rec* r, const IApplication* application, const IPropertySet* sessionProps, SHIRE& shire);

namespace {
    char* g_szSHIBConfig = NULL;
    char* g_szSchemaDir = NULL;
    ShibTargetConfig* g_Config = NULL;
    bool g_bApacheConf = false;
}

// per-dir module configuration structure
struct shire_dir_config
{
    // RM Configuration
    char* szAuthGrpFile;    // Auth GroupFile name
    int bRequireAll;        // all require directives must match, otherwise OR logic

    // SHIRE Configuration
    int bBasicHijack;       // activate for AuthType Basic?
    int bRequireSession;    // require a session?
    int bExportAssertion;   // export SAML assertion to the environment?
};

// creates per-directory config structure
extern "C" void* create_shire_dir_config (pool* p, char* d)
{
    shire_dir_config* dc=(shire_dir_config*)ap_pcalloc(p,sizeof(shire_dir_config));
    dc->bBasicHijack = -1;
    dc->bRequireSession = -1;
    dc->bExportAssertion = -1;
    dc->bRequireAll = -1;
    dc->szAuthGrpFile = NULL;
    return dc;
}

// overrides server configuration in directories
extern "C" void* merge_shire_dir_config (pool* p, void* base, void* sub)
{
    shire_dir_config* dc=(shire_dir_config*)ap_pcalloc(p,sizeof(shire_dir_config));
    shire_dir_config* parent=(shire_dir_config*)base;
    shire_dir_config* child=(shire_dir_config*)sub;

    if (child->szAuthGrpFile)
        dc->szAuthGrpFile=ap_pstrdup(p,child->szAuthGrpFile);
    else if (parent->szAuthGrpFile)
        dc->szAuthGrpFile=ap_pstrdup(p,parent->szAuthGrpFile);
    else
        dc->szAuthGrpFile=NULL;

    dc->bBasicHijack=((child->bBasicHijack==-1) ? parent->bBasicHijack : child->bBasicHijack);
    dc->bRequireSession=((child->bRequireSession==-1) ? parent->bRequireSession : child->bRequireSession);
    dc->bExportAssertion=((child->bExportAssertion==-1) ? parent->bExportAssertion : child->bExportAssertion);
    dc->bRequireAll=((child->bRequireAll==-1) ? parent->bRequireAll : child->bRequireAll);
    return dc;
}

// generic global slot handlers
extern "C" const char* ap_set_global_string_slot(cmd_parms* parms, void*, const char* arg)
{
    *((char**)(parms->info))=ap_pstrdup(parms->pool,arg);
    return NULL;
}

typedef const char* (*config_fn_t)(void);

// SHIRE Module commands

static command_rec shire_cmds[] = {
  {"SHIREConfig", (config_fn_t)ap_set_global_string_slot, &g_szSHIBConfig,
   RSRC_CONF, TAKE1, "Path to shibboleth.xml config file."},
  {"ShibConfig", (config_fn_t)ap_set_global_string_slot, &g_szSHIBConfig,
   RSRC_CONF, TAKE1, "Path to shibboleth.xml config file."},
  {"ShibSchemaDir", (config_fn_t)ap_set_global_string_slot, &g_szSchemaDir,
   RSRC_CONF, TAKE1, "Path to Shibboleth XML schema directory."},

  {"ShibBasicHijack", (config_fn_t)ap_set_flag_slot,
   (void *) XtOffsetOf (shire_dir_config, bBasicHijack),
   OR_AUTHCFG, FLAG, "Respond to AuthType Basic and convert to shib?"},
  {"ShibRequireSession", (config_fn_t)ap_set_flag_slot,
   (void *) XtOffsetOf (shire_dir_config, bRequireSession),
   OR_AUTHCFG, FLAG, "Initiates a new session if one does not exist."},
  {"ShibExportAssertion", (config_fn_t)ap_set_flag_slot,
   (void *) XtOffsetOf (shire_dir_config, bExportAssertion),
   OR_AUTHCFG, FLAG, "Export SAML assertion to Shibboleth-defined header?"},
  {"AuthGroupFile", (config_fn_t)ap_set_file_slot,
   (void *) XtOffsetOf (shire_dir_config, szAuthGrpFile),
   OR_AUTHCFG, TAKE1, "text file containing group names and member user IDs"},
  {"ShibRequireAll", (config_fn_t)ap_set_flag_slot,
   (void *) XtOffsetOf (shire_dir_config, bRequireAll),
   OR_AUTHCFG, FLAG, "All require directives must match!"},

  {NULL}
};

static const XMLCh Apache[] =
{ chLatin_A, chLatin_p, chLatin_a, chLatin_c, chLatin_h, chLatin_e, chNull };
static const XMLCh apacheConfig[] =
{ chLatin_a, chLatin_p, chLatin_a, chLatin_c, chLatin_h, chLatin_e,
  chLatin_C, chLatin_o, chLatin_n, chLatin_f, chLatin_i, chLatin_g, chNull
};
static const XMLCh Implementation[] =
{ chLatin_I, chLatin_m, chLatin_p, chLatin_l, chLatin_e, chLatin_m, chLatin_e, chLatin_n, chLatin_t, chLatin_a, chLatin_t, chLatin_i, chLatin_o, chLatin_n, chNull };

/* 
 * shire_child_init()
 *  Things to do when the child process is initialized.
 */
extern "C" void shire_child_init(server_rec* s, pool* p)
{
    // Initialize runtime components.

    ap_log_error(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,s,"shire_child_init() starting");

    if (g_Config) {
        ap_log_error(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,s,"shire_child_init(): already initialized!");
        exit(1);
    }

    try {
        g_Config=&ShibTargetConfig::getConfig();
        g_Config->setFeatures(
            ShibTargetConfig::Listener |
            ShibTargetConfig::Metadata |
            ShibTargetConfig::AAP |
            ShibTargetConfig::RequestMapper |
            ShibTargetConfig::SHIREExtensions
            );
        if (!g_Config->init(g_szSchemaDir,g_szSHIBConfig)) {
            ap_log_error(APLOG_MARK,APLOG_CRIT|APLOG_NOERRNO,s,"shire_child_init(): already initialized!");
            exit(1);
        }
        
        // Access the implementation-specifics for whether to use old Apache config style...
        IConfig* conf=g_Config->getINI();
        Locker locker(conf);
        const IPropertySet* props=conf->getPropertySet("SHIRE");
        if (props) {
            const DOMElement* impl=saml::XML::getFirstChildElement(
                props->getElement(),ShibTargetConfig::SHIBTARGET_NS,Implementation
                );
            if (impl && (impl=saml::XML::getFirstChildElement(impl,ShibTargetConfig::SHIBTARGET_NS,Apache))) {
                const XMLCh* flag=impl->getAttributeNS(NULL,apacheConfig);
                if (flag && (*flag==chDigit_1 || *flag==chLatin_t))
                    g_bApacheConf=true;
            }
        }
    }
    catch (...) {
        ap_log_error(APLOG_MARK,APLOG_CRIT|APLOG_NOERRNO,s,"shire_child_init() failed to initialize SHIB Target");
        exit (1);
    }

    ap_log_error(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,s,
        "shire_child_init() done, apacheConfig set to %s", g_bApacheConf ? "true" : "false");
}


/*
 * shire_child_exit()
 *  Cleanup.
 */
extern "C" void shire_child_exit(server_rec* s, pool* p)
{
    g_Config->shutdown();
    g_Config = NULL;
    ap_log_error(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,s,"shire_child_exit() done");
}

static int shire_error_page(request_rec* r, const IApplication* app, const char* page, ShibMLP& mlp)
{
    const IPropertySet* props=app->getPropertySet("Errors");
    if (props) {
        pair<bool,const char*> p=props->getString(page);
        if (p.first) {
            ifstream infile(p.second);
            if (!infile.fail()) {
                const char* res = mlp.run(infile);
                if (res) {
                    r->content_type = ap_psprintf(r->pool, "text/html");
                    ap_send_http_header(r);
                    ap_rprintf(r, res);
                    return DONE;
                }
            }
        }
    }

    ap_log_rerror(APLOG_MARK,APLOG_ERR,r,
        "shire_error_page() could not process shire error template for application %s",app->getId());
    return SERVER_ERROR;
}

extern "C" int shire_check_user(request_rec* r)
{
    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_user: ENTER");
    shire_dir_config* dc=(shire_dir_config*)ap_get_module_config(r->per_dir_config,&shire_module);

    ostringstream threadid;
    threadid << "[" << getpid() << "] shire" << '\0';
    saml::NDC ndc(threadid.str().c_str());

    // This will always be normalized, because Apache uses ap_get_server_name in this API call.
    const char* targeturl=ap_construct_url(r->pool,r->unparsed_uri,r);

    // We lock the configuration system for the duration.
    IConfig* conf=g_Config->getINI();
    Locker locker(conf);
    
    // Map request to application and content settings.
    IRequestMapper* mapper=conf->getRequestMapper();
    Locker locker2(mapper);
    IRequestMapper::Settings settings=mapper->getSettingsFromParsedURL(
        ap_http_method(r), ap_get_server_name(r), ap_get_server_port(r), r->unparsed_uri
        );
    pair<bool,const char*> application_id=settings.first->getString("applicationId");
    const IApplication* application=conf->getApplication(application_id.second);
    const IPropertySet* sessionProps=application ? application->getPropertySet("Sessions") : NULL;
    if (!application || !sessionProps) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
           "shire_check_user: unable to map request to application session settings, check configuration");
        return SERVER_ERROR;
    }
    
    // Declare SHIRE object for this request.
    SHIRE shire(application);
    
    // Get location of this application's assertion consumer service and see if this is it.
    if (strstr(targeturl,shire.getShireURL(targeturl))) {
        return shire_handler(r,application,sessionProps,shire);
    }

    // Regular access to arbitrary resource...check AuthType
    const char *auth_type=ap_auth_type(r);
    if (!auth_type)
        return DECLINED;

    if (strcasecmp(auth_type,"shibboleth")) {
        if (!strcasecmp(auth_type,"basic") && dc->bBasicHijack==1) {
            core_dir_config* conf=
                (core_dir_config*)ap_get_module_config(r->per_dir_config,
                    ap_find_linked_module("http_core.c"));
            conf->ap_auth_type="shibboleth";
        }
        else
            return DECLINED;
    }

    pair<bool,bool> requireSession = pair<bool,bool>(false,false);
    if (g_bApacheConf) {
        // By default, we will require a session.
        if (dc->bRequireSession!=0)
            requireSession.second=true;
    }
    else
        requireSession = settings.first->getBool("requireSession");

    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_user: session check for %s",targeturl);
    
    pair<bool,const char*> shib_cookie=sessionProps->getString("cookieName");
    if (!shib_cookie.first) {
        ap_log_rerror(APLOG_MARK,APLOG_CRIT|APLOG_NOERRNO,r,
		    "shire_check_user: no cookieName set for %s", application_id.second);
        return SERVER_ERROR;
    }

    // We're in charge, so check for cookie.
    const char* session_id=NULL;
    const char* cookies=ap_table_get(r->headers_in,"Cookie");

    if (cookies) {
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_user: cookies found: %s",cookies);
        if (session_id=strstr(cookies,shib_cookie.second)) {
            // Yep, we found a cookie -- pull it out (our session_id)
            session_id+=strlen(shib_cookie.second) + 1; /* Skip over the '=' */
            char* cookiebuf = ap_pstrdup(r->pool,session_id);
            char* cookieend = strchr(cookiebuf,';');
            if (cookieend)
                *cookieend = '\0';    /* Ignore anyting after a ; */
            session_id=cookiebuf;
        }
    }

    if (!session_id || !*session_id) {
        // If no session required, bail now.
        if (!requireSession.second)
            return DECLINED;

        // No acceptable cookie, and we require a session.  Generate an AuthnRequest.
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_user: no cookie found -- redirecting to WAYF");
        ap_table_setn(r->headers_out,"Location",ap_pstrdup(r->pool,shire.getAuthnRequest(targeturl)));
        return REDIRECT;
    }

    // Make sure this session is still valid.
    RPCError* status = NULL;
    ShibMLP markupProcessor(application);
    markupProcessor.insert("requestURL", targeturl);

    try {
        status = shire.sessionIsValid(session_id, r->connection->remote_ip);
    }
    catch (ShibTargetException &e) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,"shire_check_user(): %s", e.what());
        markupProcessor.insert("errorType", "Session Processing Error");
        markupProcessor.insert("errorText", e.what());
        markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
        return shire_error_page(r, application, "shire", markupProcessor);
    }
#ifndef _DEBUG
    catch (...) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,"shire_check_user(): caught unexpected error");
        markupProcessor.insert("errorType", "Session Processing Error");
        markupProcessor.insert("errorText", "Unexpected Exception");
        markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
        return shire_error_page(r, application, "shire", markupProcessor);
    }
#endif

    // Check the status
    if (status->isError()) {
        ap_log_rerror(APLOG_MARK,APLOG_INFO|APLOG_NOERRNO,r,
            "shire_check_user() session invalid: %s", status->getText());

        // If no session required, bail now.
        if (!requireSession.second)
            return DECLINED;
        else if (status->isRetryable()) {
            // Oops, session is invalid. Generate AuthnRequest.
            ap_table_setn(r->headers_out,"Location",ap_pstrdup(r->pool,shire.getAuthnRequest(targeturl)));
            delete status;
            return REDIRECT;
        }
        else {
            // return the error page to the user
            markupProcessor.insert(*status);
            delete status;
            return shire_error_page(r, application, "shire", markupProcessor);
        }
    }

    delete status;
    // set the connection authtype
    if (r->connection)
        r->connection->ap_auth_type = "shibboleth";
    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_user: session successfully verified");

    // This is code transferred in from mod_shibrm to export the attributes.
    // We could even combine the isSessionValid/getAssertions API...?

    RM rm(application);
    vector<SAMLAssertion*> assertions;
    SAMLAuthenticationStatement* sso_statement=NULL;

    try {
        status = rm.getAssertions(session_id, r->connection->remote_ip, assertions, &sso_statement);
    }
    catch (ShibTargetException &e) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,"shire_check_user(): %s", e.what());
        markupProcessor.insert("errorType", "Attribute Processing Error");
        markupProcessor.insert("errorText", e.what());
        markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
        return shire_error_page(r, application, "rm", markupProcessor);
    }
#ifndef _DEBUG
    catch (...) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,"shire_check_user(): caught unexpected error");
        markupProcessor.insert("errorType", "Attribute Processing Error");
        markupProcessor.insert("errorText", "Unexpected Exception");
        markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
        return shire_error_page(r, application, "rm", markupProcessor);
    }
#endif

    if (status->isError()) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
            "shire_check_user() getAssertions failed: %s", status->getText());

        markupProcessor.insert(*status);
        delete status;
        return shire_error_page(r, application, "rm", markupProcessor);
    }
    delete status;

    // Do we have an access control plugin?
    if (settings.second) {
        Locker acllock(settings.second);
        if (!settings.second->authorized(assertions)) {
            for (int k = 0; k < assertions.size(); k++)
                delete assertions[k];
            delete sso_statement;
            ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,"shire_check_user(): access control provider denied access");
            return shire_error_page(r, application, "access", markupProcessor);
        }
    }

    // Get the AAP providers, which contain the attribute policy info.
    Iterator<IAAP*> provs=application->getAAPProviders();

    // Clear out the list of mapped attributes
    while (provs.hasNext()) {
        IAAP* aap=provs.next();
        aap->lock();
        try {
            Iterator<const IAttributeRule*> rules=aap->getAttributeRules();
            while (rules.hasNext()) {
                const char* header=rules.next()->getHeader();
                if (header)
                    ap_table_unset(r->headers_in,header);
            }
        }
        catch(...) {
            aap->unlock();
            for (int k = 0; k < assertions.size(); k++)
                delete assertions[k];
            delete sso_statement;
            ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
                "shire_check_user(): caught unexpected error while clearing headers");
            markupProcessor.insert("errorType", "Attribute Processing Error");
            markupProcessor.insert("errorText", "Unexpected Exception");
            markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
            return shire_error_page(r, application, "rm", markupProcessor);
        }
        aap->unlock();
    }
    provs.reset();
    
    // Maybe export the first assertion.
    ap_table_unset(r->headers_in,"Shib-Attributes");
    pair<bool,bool> exp=pair<bool,bool>(false,false);
    if (g_bApacheConf && dc->bExportAssertion==1)
        exp.second=exp.first=true;
    else if (!g_bApacheConf)
        exp=settings.first->getBool("exportAssertion");
    if (exp.first && exp.second && assertions.size()) {
        string assertion;
        RM::serialize(*(assertions[0]), assertion);
        ap_table_set(r->headers_in,"Shib-Attributes", assertion.c_str());
    }

    // Export the SAML AuthnMethod and the origin site name.
    ap_table_unset(r->headers_in,"Shib-Origin-Site");
    ap_table_unset(r->headers_in,"Shib-Authentication-Method");
    if (sso_statement) {
        auto_ptr_char os(sso_statement->getSubject()->getNameQualifier());
        auto_ptr_char am(sso_statement->getAuthMethod());
        ap_table_set(r->headers_in,"Shib-Origin-Site", os.get());
        ap_table_set(r->headers_in,"Shib-Authentication-Method", am.get());
    }
    
    ap_table_unset(r->headers_in,"Shib-Application-ID");
    ap_table_set(r->headers_in,"Shib-Application-ID",application_id.second);

    // Export the attributes.
    Iterator<SAMLAssertion*> a_iter(assertions);
    while (a_iter.hasNext()) {
        SAMLAssertion* assert=a_iter.next();
        Iterator<SAMLStatement*> statements=assert->getStatements();
        while (statements.hasNext()) {
            SAMLAttributeStatement* astate=dynamic_cast<SAMLAttributeStatement*>(statements.next());
            if (!astate)
                continue;
            Iterator<SAMLAttribute*> attrs=astate->getAttributes();
            while (attrs.hasNext()) {
                SAMLAttribute* attr=attrs.next();
        
                // Are we supposed to export it?
                AAP wrapper(provs,attr->getName(),attr->getNamespace());
                if (wrapper.fail())
                    continue;
                
                Iterator<string> vals=attr->getSingleByteValues();
                if (!strcmp(wrapper->getHeader(),"REMOTE_USER") && vals.hasNext())
                    r->connection->user=ap_pstrdup(r->pool,vals.next().c_str());
                else {
                    int it=0;
                    char* header = (char*)ap_table_get(r->headers_in, wrapper->getHeader());
                    if (header) {
                        header=ap_pstrdup(r->pool, header);
                        it++;
                    }
                    else
                        header = ap_pstrdup(r->pool, "");
                    for (; vals.hasNext(); it++) {
                        string value = vals.next();
                        for (string::size_type pos = value.find_first_of(";", string::size_type(0));
                                pos != string::npos;
                                pos = value.find_first_of(";", pos)) {
                            value.insert(pos, "\\");
                            pos += 2;
                        }
                        header=ap_pstrcat(r->pool, header, (it ? ";" : ""), value.c_str(), NULL);
                    }
                    ap_table_setn(r->headers_in, wrapper->getHeader(), header);
               }
            }
        }
    }

    // clean up memory
    for (int k = 0; k < assertions.size(); k++)
        delete assertions[k];
    delete sso_statement;

    return OK;
}

extern "C" int shire_post_handler(request_rec* r)
{
    ostringstream threadid;
    threadid << "[" << getpid() << "] shire_post_handler" << '\0';
    saml::NDC ndc(threadid.str().c_str());

    // We lock the configuration system for the duration.
    IConfig* conf=g_Config->getINI();
    Locker locker(conf);
    
    // Map request to application and content settings.
    IRequestMapper* mapper=conf->getRequestMapper();
    Locker locker2(mapper);
    IRequestMapper::Settings settings=mapper->getSettingsFromParsedURL(
        ap_http_method(r), ap_get_server_name(r), ap_get_server_port(r), r->unparsed_uri
        );
    pair<bool,const char*> application_id=settings.first->getString("applicationId");
    const IApplication* application=conf->getApplication(application_id.second);
    const IPropertySet* sessionProps=application ? application->getPropertySet("Sessions") : NULL;
    if (!application || !sessionProps) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
           "shire_post_handler: unable to map request to application session settings, check configuration");
        return SERVER_ERROR;
    }
    
    // Declare SHIRE object for this request.
    SHIRE shire(application);
    
    return shire_handler(r, application, sessionProps, shire);
}

int shire_handler(request_rec* r, const IApplication* application, const IPropertySet* sessionProps, SHIRE& shire)
{
    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_handler() ENTER");

    // Prime the pump...
    const char* targeturl = ap_construct_url(r->pool,r->unparsed_uri,r);
    shire.getShireURL(targeturl);

    pair<bool,const char*> shib_cookie=sessionProps->getString("cookieName");
    pair<bool,const char*> shib_cookie_props=sessionProps->getString("cookieProps");
    if (!shib_cookie.first) {
        ap_log_rerror(APLOG_MARK,APLOG_CRIT|APLOG_NOERRNO,r,
            "shire_handler: no cookieName set for %s", application->getId());
        return SERVER_ERROR;
    }

    ShibMLP markupProcessor(application);
    markupProcessor.insert("requestURL", targeturl);
  
    // Process SHIRE request
    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r, "shire_handler() Beginning SHIRE processing");
      
    try {
        pair<bool,bool> shireSSL=sessionProps->getBool("shireSSL");
      
        // Make sure this is SSL, if it should be
        if ((!shireSSL.first || shireSSL.second) && strcmp(ap_http_method(r),"https"))
            throw ShibTargetException(SHIBRPC_OK, "blocked non-SSL access to session creation service");
    
        // If this is a GET, we manufacture an AuthnRequest.
        if (!strcasecmp(r->method,"GET")) {
            const char* areq=r->args ? shire.getLazyAuthnRequest(r->args) : NULL;
            if (!areq)
                throw ShibTargetException(SHIBRPC_OK, "malformed arguments to request a new session");
            ap_table_setn(r->headers_out, "Location", ap_pstrdup(r->pool,areq));
            return REDIRECT;
        }
        else if (strcasecmp(r->method,"POST")) {
            throw ShibTargetException(SHIBRPC_OK, "blocked non-POST to SHIRE POST processor");
        }
        
        // Sure sure this POST is an appropriate content type
        const char *ct = ap_table_get(r->headers_in, "Content-type");
        if (!ct || strcasecmp(ct, "application/x-www-form-urlencoded"))
            throw ShibTargetException(SHIBRPC_OK,
                ap_psprintf(r->pool, "blocked bad content-type to SHIRE POST processor: %s", (ct ? ct : "")));
    	
        // Read the posted data
        if (ap_setup_client_block(r, REQUEST_CHUNKED_ERROR))
            throw ShibTargetException(SHIBRPC_OK, "CGI setup_client_block failed");
        if (!ap_should_client_block(r))
            throw ShibTargetException(SHIBRPC_OK, "CGI should_client_block failed");
        if (r->remaining > 1024*1024)
            throw ShibTargetException (SHIBRPC_OK, "CGI length too long...");
        string cgistr;
        char buff[HUGE_STRING_LEN];
        ap_hard_timeout("mod_shire", r);
        memset(buff, 0, sizeof(buff));
        while (ap_get_client_block(r, buff, sizeof(buff)-1) > 0) {
            cgistr += buff;
            memset(buff, 0, sizeof(buff));
        }
        ap_kill_timeout(r);

        // Parse the submission.
        pair<const char*,const char*> elements=shire.getFormSubmission(cgistr.c_str());
    
        // Make sure the SAML Response parameter exists
        if (!elements.first || !*elements.first)
            throw ShibTargetException(SHIBRPC_OK, "SHIRE POST failed to find SAMLResponse form element");
    
        // Make sure the target parameter exists
        if (!elements.second || !*elements.second)
            throw ShibTargetException(SHIBRPC_OK, "SHIRE POST failed to find TARGET form element");
    
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
            "shire_handler() Processing POST for target: %s", elements.second);

        // process the post
        string cookie;
        RPCError* status = shire.sessionCreate(elements.first, r->connection->remote_ip, cookie);
    
        if (status->isError()) {
            ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
    		    "shire_handler() POST process failed (%d): %s", status->getCode(), status->getText());
    
            if (status->isRetryable()) {
                delete status;
                ap_log_rerror(APLOG_MARK,APLOG_INFO|APLOG_NOERRNO,r,
        	        "shire_handler() retryable error, generating new AuthnRequest");
                ap_table_setn(r->headers_out,"Location",ap_pstrdup(r->pool,shire.getAuthnRequest(elements.second)));
                return REDIRECT;
            }
    
            // return this error to the user.
            markupProcessor.insert(*status);
            delete status;
            return shire_error_page(r, application, "shire", markupProcessor);
        }
        delete status;
    
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
    		  "shire_handler() POST process succeeded.  New session: %s", cookie.c_str());
    
        // We've got a good session, set the cookie...
        char* val = ap_psprintf(r->pool,"%s=%s%s",shib_cookie.second,cookie.c_str(),
            shib_cookie_props.first ? shib_cookie_props.second : "");
        ap_table_setn(r->err_headers_out, "Set-Cookie", val);
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r, "shire_handler() setting cookie: %s", val);
    		    
        // ... and redirect to the target
        ap_table_setn(r->headers_out, "Location", ap_pstrdup(r->pool,elements.second));
        return REDIRECT;
    }
    catch (ShibTargetException &e) {
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r, "shire_handler() caught exception: %s", e.what());
        markupProcessor.insert("errorType", "Session Creation Service Error");
        markupProcessor.insert("errorText", e.what());
        markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
        return shire_error_page(r, application, "shire", markupProcessor);
    }
#ifndef _DEBUG
    catch (...) {
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_handler(): unexpected exception");
        markupProcessor.insert("errorType", "Session Creation Service Error");
        markupProcessor.insert("errorText", "Unknown Exception");
        markupProcessor.insert("errorDesc", "An error occurred while processing your request.");
        return shire_error_page(r, application, "shire", markupProcessor);
    }
#endif
    ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,"shire_handler() server error");
    return SERVER_ERROR;
}

static table* groups_for_user(request_rec* r, const char* user, char* grpfile)
{
    configfile_t* f;
    table* grps=ap_make_table(r->pool,15);
    char l[MAX_STRING_LEN];
    const char *group_name, *ll, *w;

    if (!(f=ap_pcfg_openfile(r->pool,grpfile))) {
        ap_log_rerror(APLOG_MARK,APLOG_DEBUG,r,"groups_for_user() could not open group file: %s\n",grpfile);
        return NULL;
    }

    pool* sp=ap_make_sub_pool(r->pool);

    while (!(ap_cfg_getline(l,MAX_STRING_LEN,f))) {
        if ((*l=='#') || (!*l))
            continue;
        ll = l;
        ap_clear_pool(sp);

        group_name=ap_getword(sp,&ll,':');

        while (*ll) {
            w=ap_getword_conf(sp,&ll);
            if (!strcmp(w,user)) {
                ap_table_setn(grps,ap_pstrdup(r->pool,group_name),"in");
                break;
            }
        }
    }
    ap_cfg_closefile(f);
    ap_destroy_pool(sp);
    return grps;
}

extern "C" int shire_check_auth(request_rec* r)
{
    shire_dir_config* dc=
        (shire_dir_config*)ap_get_module_config(r->per_dir_config,&shire_module);

    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_auth() executing");

    // Regular access to arbitrary resource...check AuthType
    const char* auth_type=ap_auth_type(r);
    if (!auth_type || strcasecmp(auth_type,"shibboleth"))
        return DECLINED;

    ostringstream threadid;
    threadid << "[" << getpid() << "] shibrm" << '\0';
    saml::NDC ndc(threadid.str().c_str());

    // We lock the configuration system for the duration.
    IConfig* conf=g_Config->getINI();
    Locker locker(conf);
    
    const char* application_id=ap_table_get(r->headers_in,"Shib-Application-ID");
    if (!application_id) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
           "shire_check_auth: Shib-Application-ID header not found in request");
        return FORBIDDEN;
    }

    const IApplication* application=conf->getApplication(application_id);
    if (!application) {
        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
           "shire_check_auth: unable to map request to application settings, check configuration");
        return SERVER_ERROR;
    }

    // mod_auth clone

    int m=r->method_number;
    bool method_restricted=false;
    const char *t, *w;
    
    const array_header* reqs_arr=ap_requires(r);
    if (!reqs_arr)
        return OK;

    require_line* reqs=(require_line*)reqs_arr->elts;

    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"REQUIRE nelts: %d", reqs_arr->nelts);
    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"REQUIRE all: %d", dc->bRequireAll);

    vector<bool> auth_OK(reqs_arr->nelts,false);

#define SHIB_AP_CHECK_IS_OK {       \
     if (dc->bRequireAll < 1)    \
         return OK;      \
     auth_OK[x] = true;      \
     continue;           \
}

    for (int x=0; x<reqs_arr->nelts; x++) {
        auth_OK[x] = false;
        if (!(reqs[x].method_mask & (1 << m)))
            continue;
        method_restricted=true;

        t = reqs[x].requirement;
        w = ap_getword_white(r->pool, &t);

        if (!strcmp(w,"valid-user")) {
            ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_auth() accepting valid-user");
            SHIB_AP_CHECK_IS_OK;
        }
        else if (!strcmp(w,"user") && r->connection->user) {
            bool regexp=false;
            while (*t) {
                w=ap_getword_conf(r->pool,&t);
                if (*w=='~') {
                    regexp=true;
                    continue;
                }
                
                if (regexp) {
                    try {
                        // To do regex matching, we have to convert from UTF-8.
                        auto_ptr<XMLCh> trans(fromUTF8(w));
                        RegularExpression re(trans.get());
                        auto_ptr<XMLCh> trans2(fromUTF8(r->connection->user));
                        if (re.matches(trans2.get())) {
                            ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_auth() accepting user: %s",w);
                            SHIB_AP_CHECK_IS_OK;
                        }
                    }
                    catch (XMLException& ex) {
                        auto_ptr_char tmp(ex.getMessage());
                        ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
                                        "shire_check_auth caught exception while parsing regular expression (%s): %s",w,tmp.get());
                    }
                }
                else if (!strcmp(r->connection->user,w)) {
                    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_auth() accepting user: %s",w);
                    SHIB_AP_CHECK_IS_OK;
                }
            }
        }
        else if (!strcmp(w,"group")) {
            table* grpstatus=NULL;
            if (dc->szAuthGrpFile && r->connection->user) {
                ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_auth() using groups file: %s\n",dc->szAuthGrpFile);
                grpstatus=groups_for_user(r,r->connection->user,dc->szAuthGrpFile);
            }
            if (!grpstatus)
                return DECLINED;
    
            while (*t) {
                w=ap_getword_conf(r->pool,&t);
                if (ap_table_get(grpstatus,w)) {
                    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,"shire_check_auth() accepting group: %s",w);
                    SHIB_AP_CHECK_IS_OK;
                }
            }
        }
        else {
            Iterator<IAAP*> provs=application->getAAPProviders();
            AAP wrapper(provs,w);
            if (wrapper.fail()) {
                ap_log_rerror(APLOG_MARK,APLOG_WARNING|APLOG_NOERRNO,r,
                    "shire_check_auth() didn't recognize require rule: %s\n",w);
                continue;
            }

            bool regexp=false;
            const char* vals=ap_table_get(r->headers_in,wrapper->getHeader());
            while (*t && vals) {
                w=ap_getword_conf(r->pool,&t);
                if (*w=='~') {
                    regexp=true;
                    continue;
                }

                try {
                    auto_ptr<RegularExpression> re;
                    if (regexp) {
                        delete re.release();
                        auto_ptr<XMLCh> trans(fromUTF8(w));
                        auto_ptr<RegularExpression> temp(new RegularExpression(trans.get()));
                        re=temp;
                    }
                    
                    string vals_str(vals);
                    int j = 0;
                    for (int i = 0;  i < vals_str.length();  i++) {
                        if (vals_str.at(i) == ';') {
                            if (i == 0) {
                                ap_log_rerror(APLOG_MARK,APLOG_WARNING|APLOG_NOERRNO,r,
                                                "shire_check_auth() invalid header encoding %s: starts with semicolon", vals);
                                return SERVER_ERROR;
                            }
    
                            if (vals_str.at(i-1) == '\\') {
                                vals_str.erase(i-1, 1);
                                i--;
                                continue;
                            }
    
                            string val = vals_str.substr(j, i-j);
                            j = i+1;
                            if (regexp) {
                                auto_ptr<XMLCh> trans(fromUTF8(val.c_str()));
                                if (re->matches(trans.get())) {
                                    ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
                                                    "shire_check_auth() expecting %s, got %s: authorization granted", w, val.c_str());
                                    SHIB_AP_CHECK_IS_OK;
                                }
                            }
                            else if (val==w) {
                                ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
                                                "shire_check_auth() expecting %s, got %s: authorization granted", w, val.c_str());
                                SHIB_AP_CHECK_IS_OK;
                            }
                            else {
                                ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
                                                "shire_check_auth() expecting %s, got %s: authorization not granted", w, val.c_str());
                            }
                        }
                    }
    
                    string val = vals_str.substr(j, vals_str.length()-j);
                    if (regexp) {
                        auto_ptr<XMLCh> trans(fromUTF8(val.c_str()));
                        if (re->matches(trans.get())) {
                            ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
                                            "shire_check_auth() expecting %s, got %s: authorization granted", w, val.c_str());
                            SHIB_AP_CHECK_IS_OK;
                        }
                    }
                    else if (val==w) {
                        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
                                        "shire_check_auth() expecting %s, got %s: authorization granted", w, val.c_str());
                        SHIB_AP_CHECK_IS_OK;
                    }
                    else {
                        ap_log_rerror(APLOG_MARK,APLOG_DEBUG|APLOG_NOERRNO,r,
                                        "shire_check_auth() expecting %s, got %s: authorization not granted", w, val.c_str());
                    }
                }
                catch (XMLException& ex) {
                    auto_ptr_char tmp(ex.getMessage());
                    ap_log_rerror(APLOG_MARK,APLOG_ERR|APLOG_NOERRNO,r,
                                    "shire_check_auth caught exception while parsing regular expression (%s): %s",w,tmp.get());
                }
            }
        }
    }

    // check if all require directives are true
    bool auth_all_OK = true;
    for (int i= 0; i<reqs_arr->nelts; i++) {
        auth_all_OK &= auth_OK[i];
    }
    if (auth_all_OK)
        return OK;

    if (!method_restricted)
        return OK;

    ShibMLP markupProcessor(application);
    markupProcessor.insert("requestURL", ap_construct_url(r->pool,r->unparsed_uri,r));
    return shire_error_page(r, application, "access", markupProcessor);
}


extern "C"{
handler_rec shire_handlers[] = {
  { "shib-shire-post", shire_post_handler },
  { NULL }
};

module MODULE_VAR_EXPORT shire_module = {
    STANDARD_MODULE_STUFF,
    NULL,                        /* initializer */
    create_shire_dir_config,	/* dir config creater */
    merge_shire_dir_config,	/* dir merger --- default is to override */
    NULL,	                /* server config */
    NULL,	                /* merge server config */
    shire_cmds,			/* command table */
    shire_handlers,		/* handlers */
    NULL,			/* filename translation */
    shire_check_user,	/* check_user_id */
    shire_check_auth,	/* check auth */
    NULL,			/* check access */
    NULL,			/* type_checker */
    NULL,			/* fixups */
    NULL,			/* logger */
    NULL,			/* header parser */
    shire_child_init,		/* child_init */
    shire_child_exit,		/* child_exit */
    NULL			/* post read-request */
};
}
