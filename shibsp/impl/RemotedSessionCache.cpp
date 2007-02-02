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
 * RemotedSessionCache.cpp
 * 
 * SessionCache implementation that delegates to a remoted version.
 */

#include "internal.h"
#include "Application.h"
#include "exceptions.h"
#include "ServiceProvider.h"
#include "SessionCache.h"
#include "TransactionLog.h"
#include "attribute/Attribute.h"
#include "remoting/ListenerService.h"
#include "util/SPConstants.h"

#include <sstream>
#include <log4cpp/Category.hh>
#include <xmltooling/XMLToolingConfig.h>
#include <xmltooling/util/NDC.h>
#include <xmltooling/util/XMLHelper.h>

using namespace shibsp;
using namespace opensaml::saml2md;
using namespace opensaml;
using namespace xmltooling;
using namespace log4cpp;
using namespace std;

namespace shibsp {

    class RemotedCache;
    class RemotedSession : public virtual Session
    {
    public:
        RemotedSession(RemotedCache* cache, const Application& app, DDF& obj)
                : m_appId(app.getId()), m_version(obj["version"].integer()), m_obj(obj),
                    m_nameid(NULL), m_expires(0), m_lastAccess(time(NULL)), m_cache(cache), m_lock(NULL) {
            const char* nameid = obj["nameid"].string();
            if (!nameid)
                throw FatalProfileException("NameID missing from remotely cached session.");
            
            // Parse and bind the document into an XMLObject.
            istringstream instr(nameid);
            DOMDocument* doc = XMLToolingConfig::getConfig().getParser().parse(instr); 
            XercesJanitor<DOMDocument> janitor(doc);
            auto_ptr<saml2::NameID> n(saml2::NameIDBuilder::buildNameID());
            n->unmarshall(doc->getDocumentElement(), true);
            janitor.release();
            
            // TODO: Process attributes...

            auto_ptr_XMLCh exp(m_obj["expires"].string());
            if (exp.get()) {
                DateTime iso(exp.get());
                iso.parseDateTime();
                m_expires = iso.getEpoch();
            }

            m_nameid = n.release();
            m_lock = Mutex::create();
        }
        
        ~RemotedSession() {
            delete m_lock;
            m_obj.destroy();
            delete m_nameid;
            for_each(m_attributes.begin(), m_attributes.end(), xmltooling::cleanup<Attribute>());
            for_each(m_tokens.begin(), m_tokens.end(), xmltooling::cleanup_pair<string,RootObject>());
        }
        
        Lockable* lock() {
            m_lock->lock();
            return this;
        }
        void unlock() {
            m_lock->unlock();
        }
        
        const char* getClientAddress() const {
            return m_obj["client_addr"].string();
        }
        const char* getEntityID() const {
            return m_obj["entity_id"].string();
        }
        const char* getAuthnInstant() const {
            return m_obj["authn_instant"].string();
        }
        const opensaml::saml2::NameID& getNameID() const {
            return *m_nameid;
        }
        const char* getSessionIndex() const {
            return m_obj["session_index"].string();
        }
        const char* getAuthnContextClassRef() const {
            return m_obj["authncontext_class"].string();
        }
        const char* getAuthnContextDeclRef() const {
            return m_obj["authncontext_decl"].string();
        }
        const vector<const Attribute*>& getAttributes() const {
            return m_attributes;
        }
        const vector<const char*>& getAssertionIDs() const {
            if (m_ids.empty()) {
                DDF id = m_obj["assertions"].first();
                while (id.isstring()) {
                    m_ids.push_back(id.name());
                    id = id.next();
                }
            }
            return m_ids;
        }
        
        const RootObject* getAssertion(const char* id) const;

        void addAttributes(const vector<Attribute*>& attributes) {
            throw ConfigurationException("addAttributes method not implemented by this session cache plugin.");
        }
        void addAssertion(RootObject* assertion) {
            throw ConfigurationException("addAttributes method not implemented by this session cache plugin.");
        }

        time_t expires() const { return m_expires; }
        time_t lastAccess() const { return m_lastAccess; }
        void validate(const Application& application, const char* client_addr, time_t timeout, bool local=true);

    private:
        string m_appId;
        int m_version;
        mutable DDF m_obj;
        saml2::NameID* m_nameid;
        vector<const Attribute*> m_attributes;
        mutable vector<const char*> m_ids;
        mutable map<string,RootObject*> m_tokens;
        time_t m_expires,m_lastAccess;
        RemotedCache* m_cache;
        Mutex* m_lock;
    };
    
    class RemotedCache : public SessionCache
    {
    public:
        RemotedCache(const DOMElement* e);
        ~RemotedCache();
    
        string insert(
            time_t expires,
            const Application& application,
            const char* client_addr,
            const saml2md::EntityDescriptor* issuer,
            const saml2::NameID& nameid,
            const char* authn_instant=NULL,
            const char* session_index=NULL,
            const char* authncontext_class=NULL,
            const char* authncontext_decl=NULL,
            const RootObject* ssoToken=NULL,
            const vector<Attribute*>* attributes=NULL
            );
        Session* find(const char* key, const Application& application, const char* client_addr=NULL, time_t timeout=0);
        void remove(const char* key, const Application& application, const char* client_addr);
        
        void cleanup();
    
        Category& m_log;
    private:
        const DOMElement* m_root;         // Only valid during initialization
        RWLock* m_lock;
        map<string,RemotedSession*> m_hashtable;
    
        void dormant(const char* key);
        static void* cleanup_fn(void*);
        bool shutdown;
        CondWait* shutdown_wait;
        Thread* cleanup_thread;
    };

    SessionCache* SHIBSP_DLLLOCAL RemotedCacheFactory(const DOMElement* const & e)
    {
        return new RemotedCache(e);
    }
}

const RootObject* RemotedSession::getAssertion(const char* id) const
{
    map<string,RootObject*>::const_iterator i = m_tokens.find(id);
    if (i!=m_tokens.end())
        return i->second;

    const char* tokenstr = m_obj["assertions"][id].string();
    if (!tokenstr)
        return NULL;
    
    // Parse and bind the document into an XMLObject.
    istringstream instr(tokenstr);
    DOMDocument* doc = XMLToolingConfig::getConfig().getParser().parse(instr); 
    XercesJanitor<DOMDocument> janitor(doc);
    auto_ptr<XMLObject> xmlObject(XMLObjectBuilder::buildOneFromElement(doc->getDocumentElement(), true));
    janitor.release();
    
    RootObject* token = dynamic_cast<RootObject*>(xmlObject.get());
    if (!token || !token->isAssertion())
        throw FatalProfileException("Cached assertion was of an unknown object type.");

    // Transfer ownership to us.
    xmlObject.release();
    m_tokens[id]=token;
    return token;
}

void RemotedSession::validate(const Application& application, const char* client_addr, time_t timeout, bool local)
{
    // Basic expiration?
    time_t now = time(NULL);
    if (now > m_expires) {
        m_cache->m_log.info("session expired (ID: %s)", m_obj.name());
        RetryableProfileException ex("Your session has expired, and you must re-authenticate.");
        if (!getEntityID())
            throw ex;
        MetadataProvider* m=application.getMetadataProvider();
        Locker locker(m);
        annotateException(&ex,m->getEntityDescriptor(getEntityID(),false)); // throws it
    }

    // Address check?
    if (client_addr) {
        if (m_cache->m_log.isDebugEnabled())
            m_cache->m_log.debug("comparing client address %s against %s", client_addr, getClientAddress());
        if (strcmp(getClientAddress(),client_addr)) {
            m_cache->m_log.warn("client address mismatch");
            RetryableProfileException ex(
                "Your IP address ($1) does not match the address recorded at the time the session was established.",
                params(1,client_addr)
                );
            if (!getEntityID())
                throw ex;
            MetadataProvider* m=application.getMetadataProvider();
            Locker locker(m);
            annotateException(&ex,m->getEntityDescriptor(getEntityID(),false)); // throws it
        }
    }

    if (local)
        return;
    
    DDF in("touch::"REMOTED_SESSION_CACHE), out;
    DDFJanitor jin(in);
    in.structure();
    in.addmember("key").string(m_obj.name());
    in.addmember("application_id").string(m_appId.c_str());
    in.addmember("version").integer(m_obj["version"].integer());
    if (timeout) {
        // On 64-bit Windows, time_t doesn't fit in a long, so I'm using ISO timestamps.  
#ifndef HAVE_GMTIME_R
        struct tm* ptime=gmtime(&timeout);
#else
        struct tm res;
        struct tm* ptime=gmtime_r(&timeout,&res);
#endif
        char timebuf[32];
        strftime(timebuf,32,"%Y-%m-%dT%H:%M:%SZ",ptime);
        in.addmember("timeout").string(timebuf);
    }

    try {
        out=SPConfig::getConfig().getServiceProvider()->getListenerService()->send(in);
        if (out.isstruct()) {
            // We got an updated record back.
            m_ids.clear();
            for_each(m_attributes.begin(), m_attributes.end(), xmltooling::cleanup<Attribute>());
            m_attributes.clear();
            m_obj.destroy();
            m_obj = out;
            
            // TODO: handle attributes
        }
    }
    catch (...) {
        out.destroy();
        throw;
    }
    
    m_lastAccess = now;
}

RemotedCache::RemotedCache(const DOMElement* e)
    : SessionCache(e), m_log(Category::getInstance(SHIBSP_LOGCAT".SessionCache")), m_root(e), m_lock(NULL), shutdown(false)
{
    if (!SPConfig::getConfig().getServiceProvider()->getListenerService())
        throw ConfigurationException("RemotedCacheService requires a ListenerService, but none available.");
        
    m_lock = RWLock::create();
    shutdown_wait = CondWait::create();
    cleanup_thread = Thread::create(&cleanup_fn, (void*)this);
}

RemotedCache::~RemotedCache()
{
    // Shut down the cleanup thread and let it know...
    shutdown = true;
    shutdown_wait->signal();
    cleanup_thread->join(NULL);

    for_each(m_hashtable.begin(),m_hashtable.end(),xmltooling::cleanup_pair<string,RemotedSession>());
    delete m_lock;
    delete shutdown_wait;
}

string RemotedCache::insert(
    time_t expires,
    const Application& application,
    const char* client_addr,
    const saml2md::EntityDescriptor* issuer,
    const saml2::NameID& nameid,
    const char* authn_instant,
    const char* session_index,
    const char* authncontext_class,
    const char* authncontext_decl,
    const RootObject* ssoToken,
    const vector<Attribute*>* attributes
    )
{
    DDF in("insert::"REMOTED_SESSION_CACHE);
    DDFJanitor jin(in);
    in.structure();
    if (expires) {
#ifndef HAVE_GMTIME_R
        struct tm* ptime=gmtime(&expires);
#else
        struct tm res;
        struct tm* ptime=gmtime_r(&expires,&res);
#endif
        char timebuf[32];
        strftime(timebuf,32,"%Y-%m-%dT%H:%M:%SZ",ptime);
        in.addmember("expires").string(timebuf);
    }
    in.addmember("application_id").string(application.getId());
    in.addmember("client_addr").string(client_addr);
    if (issuer) {
        auto_ptr_char provid(issuer->getEntityID());
        in.addmember("entity_id").string(provid.get());
    }
    if (authn_instant)
        in.addmember("authn_instant").string(authn_instant);
    if (session_index)
        in.addmember("session_index").string(session_index);
    if (authncontext_class)
        in.addmember("authncontext_class").string(authncontext_class);
    if (authncontext_decl)
        in.addmember("authncontext_decl").string(authncontext_decl);
    
    ostringstream namestr;
    namestr << nameid;
    in.addmember("nameid").string(namestr.str().c_str());

    if (ssoToken) {
        ostringstream tokenstr;
        tokenstr << *ssoToken;
        auto_ptr_char tokenid(ssoToken->getID());
        in.addmember("assertions").list().add(DDF(tokenid.get()).string(tokenstr.str().c_str()));
    }
    
    if (attributes) {
        DDF attr;
        DDF attrs = in.addmember("attributes").list();
        for (vector<Attribute*>::const_iterator a=attributes->begin(); a!=attributes->end(); ++a) {
            attr = (*a)->marshall();
            attrs.add(attr);
        }
    }

    DDF out=SPConfig::getConfig().getServiceProvider()->getListenerService()->send(in);
    DDFJanitor jout(out);
    if (out["key"].isstring()) {
        for_each(attributes->begin(), attributes->end(), xmltooling::cleanup<Attribute>());

        // Transaction Logging
        auto_ptr_char name(nameid.getName());
        const char* pid = in["entity_id"].string();
        TransactionLog* xlog = SPConfig::getConfig().getServiceProvider()->getTransactionLog();
        Locker locker(xlog);
        xlog->log.infoStream() <<
            "New session (ID: " <<
                out["key"].string() <<
            ") with (applicationId: " <<
                application.getId() <<
            ") for principal from (IdP: " <<
                (pid ? pid : "none") <<
            ") at (ClientAddress: " <<
                client_addr <<
            ") with (NameIdentifier: " <<
                name.get() <<
            ")";

        return out["key"].string();
    }
    throw RetryableProfileException("A remoted cache insertion operation did not return a usable session key.");
}

Session* RemotedCache::find(const char* key, const Application& application, const char* client_addr, time_t timeout)
{
#ifdef _DEBUG
    xmltooling::NDC ndc("find");
#endif

    bool localValidation = false;
    RemotedSession* session=NULL;
    m_log.debug("searching local cache for session (%s)", key);
    m_lock->rdlock();
    map<string,RemotedSession*>::const_iterator i=m_hashtable.find(key);
    if (i==m_hashtable.end()) {
        m_lock->unlock();
        m_log.debug("session not found locally, searching remote cache");

        DDF in("find::"REMOTED_SESSION_CACHE), out;
        DDFJanitor jin(in);
        in.structure();
        in.addmember("key").string(key);
        in.addmember("application_id").string(application.getId());
        if (timeout) {
            // On 64-bit Windows, time_t doesn't fit in a long, so I'm using ISO timestamps.  
#ifndef HAVE_GMTIME_R
            struct tm* ptime=gmtime(&timeout);
#else
            struct tm res;
            struct tm* ptime=gmtime_r(&timeout,&res);
#endif
            char timebuf[32];
            strftime(timebuf,32,"%Y-%m-%dT%H:%M:%SZ",ptime);
            in.addmember("timeout").string(timebuf);
        }
        
        try {
            out=SPConfig::getConfig().getServiceProvider()->getListenerService()->send(in);
            if (!out.isstruct()) {
                out.destroy();
                m_log.debug("session not found in remote cache");
                return NULL;
            }
            
            // Wrap the results in a local entry and save it.
            session = new RemotedSession(this, application, out);
            // The remote end has handled timeout issues, we handle address and expiration checks.
            localValidation = true;
        }
        catch (...) {
            out.destroy();
            throw;
        }

        // Lock for writing and repeat the search to avoid duplication.
        m_lock->wrlock();
        SharedLock shared(m_lock, false);
        if (m_hashtable.count(key)) {
            delete session;
            // We're using an existing session entry, so we have to switch back to full validation.
            localValidation = false;
            session = m_hashtable[key];
            session->lock();
        }
        else {
            m_hashtable[key]=session;
            session->lock();
        }
    }
    else {
        // Save off and lock the session.
        session = i->second;
        session->lock();
        m_lock->unlock();
        
        m_log.debug("session found locally, validating it for use");
    }

    // Verify currency and update the timestamp. If the local switch is false, we also update the access time.
    try {
        session->validate(application, client_addr, timeout, localValidation);
    }
    catch (...) {
        session->unlock();
        throw;
    }
    
    return session;
}

void RemotedCache::remove(const char* key, const Application& application, const char* client_addr)
{
    // Take care of local copy.
    dormant(key);
    
    // Now remote...
    DDF in("remove::"REMOTED_SESSION_CACHE);
    DDFJanitor jin(in);
    in.structure();
    in.addmember("key").string(key);
    in.addmember("application_id").string(application.getId());
    in.addmember("client_addr").string(client_addr);
    
    DDF out = SPConfig::getConfig().getServiceProvider()->getListenerService()->send(in);
    out.destroy();
}

void RemotedCache::dormant(const char* key)
{
#ifdef _DEBUG
    xmltooling::NDC ndc("dormant");
#endif

    m_log.debug("deleting local copy of session (%s)", key);

    // lock the cache for writing, which means we know nobody is sitting in find()
    m_lock->wrlock();

    // grab the entry from the table
    map<string,RemotedSession*>::const_iterator i=m_hashtable.find(key);
    if (i==m_hashtable.end()) {
        m_lock->unlock();
        return;
    }

    // ok, remove the entry and lock it
    RemotedSession* entry=i->second;
    m_hashtable.erase(key);
    entry->lock();
    
    // unlock the cache
    m_lock->unlock();

    // we can release the cache entry lock because we know we're not in the cache anymore
    entry->unlock();

    delete entry;
}

void RemotedCache::cleanup()
{
#ifdef _DEBUG
    xmltooling::NDC ndc("cleanup");
#endif

    int rerun_timer = 0;
    Mutex* mutex = Mutex::create();
  
    // Load our configuration details...
    static const XMLCh cleanupInterval[] = UNICODE_LITERAL_15(c,l,e,a,n,u,p,I,n,t,e,r,v,a,l);
    const XMLCh* tag=m_root->getAttributeNS(NULL,cleanupInterval);
    if (tag && *tag)
        rerun_timer = XMLString::parseInt(tag);

    if (rerun_timer <= 0)
        rerun_timer = 300;        // rerun every 5 minutes

    mutex->lock();

    m_log.info("cleanup thread started...run every %d secs; timeout after %d secs", rerun_timer, m_cacheTimeout);

    while (!shutdown) {
        shutdown_wait->timedwait(mutex,rerun_timer);
        if (shutdown)
            break;

        // Ok, let's run through the cleanup process and clean out
        // really old sessions.  This is a two-pass process.  The
        // first pass is done holding a read-lock while we iterate over
        // the cache.  The second pass doesn't need a lock because
        // the 'deletes' will lock the cache.
    
        // Pass 1: iterate over the map and find all entries that have not been
        // used in X hours
        vector<string> stale_keys;
        time_t stale = time(NULL) - m_cacheTimeout;
    
        m_lock->rdlock();
        for (map<string,RemotedSession*>::const_iterator i=m_hashtable.begin(); i!=m_hashtable.end(); ++i) {
            // If the last access was BEFORE the stale timeout...
            i->second->lock();
            time_t last=i->second->lastAccess();
            i->second->unlock();
            if (last < stale)
                stale_keys.push_back(i->first);
        }
        m_lock->unlock();
    
        if (!stale_keys.empty()) {
            m_log.info("purging %d old sessions", stale_keys.size());
    
            // Pass 2: walk through the list of stale entries and remove them from the cache
            for (vector<string>::const_iterator j = stale_keys.begin(); j != stale_keys.end(); ++j)
                dormant(j->c_str());
        }
    }

    m_log.info("cleanup thread exiting");

    mutex->unlock();
    delete mutex;
    Thread::exit(NULL);
}

void* RemotedCache::cleanup_fn(void* cache_p)
{
    RemotedCache* cache = reinterpret_cast<RemotedCache*>(cache_p);

#ifndef WIN32
    // First, let's block all signals 
    Thread::mask_all_signals();
#endif

    // Now run the cleanup process.
    cache->cleanup();
    return NULL;
}

/* These are currently unimplemented.

void RemotedSession::addAttributes(const vector<Attribute*>& attributes)
{
    DDF in("addAttributes::"REMOTED_SESSION_CACHE);
    DDFJanitor jin(in);
    in.structure();
    in.addmember("key").string(m_key.c_str());
    in.addmember("application_id").string(m_appId.c_str());

    DDF attr;
    DDF attrs = in.addmember("attributes").list();
    for (vector<Attribute*>::const_iterator a=attributes.begin(); a!=attributes.end(); ++a) {
        attr = (*a)->marshall();
        attrs.add(attr);
    }

    attr=SPConfig::getConfig().getServiceProvider()->getListenerService()->send(in);
    DDFJanitor jout(attr);
    
    // Transfer ownership to us.
    m_attributes.insert(m_attributes.end(), attributes.begin(), attributes.end());
}

void RemotedSession::addAssertion(RootObject* assertion)
{
    if (!assertion || !assertion->isAssertion())
        throw FatalProfileException("Unknown object type passed to session cache for storage.");

    DDF in("addAssertion::"REMOTED_SESSION_CACHE);
    DDFJanitor jin(in);
    in.structure();
    in.addmember("key").string(m_key.c_str());
    in.addmember("application_id").string(m_appId.c_str());
    
    ostringstream os;
    os << *assertion;
    string token(os.str());
    auto_ptr_char tokenid(assertion->getID());
    in.addmember("assertion_id").string(tokenid.get());
    in.addmember("assertion").string(token.c_str());

    DDF out = SPConfig::getConfig().getServiceProvider()->getListenerService()->send(in);
    out.destroy();
    
    // Add to local record and token map.
    // Next attempt to find and lock session will refresh from remote store anyway.
    m_obj["assertions"].addmember(tokenid.get()).string(token.c_str());
    m_ids.clear();
    m_tokens[tokenid.get()] = assertion;
}

*/