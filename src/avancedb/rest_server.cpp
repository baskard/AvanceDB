#include "rest_server.h"

#include <sstream>
#include <cstring>
#include <iomanip>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "json_stream.h"
#include "rest_exceptions.h"
#include "database.h"
#include "document.h"
#include "script_object_stream.h"

#include "libscriptobject_gason.h"

#define REGEX_DBNAME R"(_?[a-z][a-z0-9_\$\+\-\(\)]+)"
#define REGEX_DBNAME_GROUP "/(?<db>" REGEX_DBNAME ")"

#define REGEX_DOCID R"([a-zA-Z0-9\$\+\-\(\)\:\.\~][a-zA-Z0-9_\$\+\-\(\)\:\.\~]*)"
#define REGEX_DOCID_GROUP "/(?<id>" REGEX_DOCID ")"

RestServer::RestServer() {
    AddRoute("HEAD", REGEX_DBNAME_GROUP "/?", &RestServer::HeadDatabase);   
    AddRoute("HEAD", REGEX_DBNAME_GROUP REGEX_DOCID_GROUP, &RestServer::HeadDocument);
    
    AddRoute("DELETE", REGEX_DBNAME_GROUP "/?/?", &RestServer::DeleteDatabase);
    AddRoute("DELETE", REGEX_DBNAME_GROUP REGEX_DOCID_GROUP, &RestServer::DeleteDocument);
    
    AddRoute("PUT", REGEX_DBNAME_GROUP REGEX_DOCID_GROUP, &RestServer::PutDocument);
    AddRoute("PUT", REGEX_DBNAME_GROUP "/?", &RestServer::PutDatabase);
    
    AddRoute("GET", "/_active_tasks", &RestServer::GetActiveTasks);
    AddRoute("GET", "/_uuids", &RestServer::GetUuids);
    AddRoute("GET", "/_session", &RestServer::GetSession);
    AddRoute("GET", "/_all_dbs", &RestServer::GetAllDbs);    
    AddRoute("GET", REGEX_DBNAME_GROUP REGEX_DOCID_GROUP, &RestServer::GetDocument);
    AddRoute("GET", REGEX_DBNAME_GROUP "/_all_docs", &RestServer::GetDatabaseAllDocs);
    AddRoute("GET", REGEX_DBNAME_GROUP "/?", &RestServer::GetDatabase);
    AddRoute("GET", "/_config/query_servers/?", &RestServer::GetConfigQueryServers);
    AddRoute("GET", "/_config/native_query_servers/?", &RestServer::GetConfigNativeQueryServers);
    AddRoute("GET", "/", &RestServer::GetSignature);
    
    databases_.AddDatabase("_replicator");
    databases_.AddDatabase("_users");
}

void RestServer::AddRoute(const char* method, const char* re, Callback func) {
    router_.Add(method, re, boost::bind(func, this, _1, _2, _3));
}

void RestServer::RouteRequest(rs::httpserver::socket_ptr, rs::httpserver::request_ptr request, rs::httpserver::response_ptr response) {
    router_.Match(request, response);
}

bool RestServer::GetActiveTasks(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    response->setContentType("application/json").Send("[]");
    return true;
}

bool RestServer::GetSession(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    response->setContentType("application/json").Send(R"({"ok":true,"userCtx":{"name":null,"roles":["_admin"]},"info":{"authentication_db":"_users","authentication_handlers":["oauth","cookie","default"],"authenticated":"default"}})");
    return true;
}

bool RestServer::GetAllDbs(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    auto dbs = databases_.GetDatabases();
    
    std::stringstream stream;
    stream << "[";
    for (int i = 0; i < dbs.size(); ++i) {
        stream << (i > 0 ? "," : "") << "\"" << dbs[i] << "\"";
    }
    stream << "]";
    
    response->setContentType("application/json").Send(stream.str());
    return true;
}

bool RestServer::GetSignature(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    response->setContentType("application/json").Send(R"({"couchdb":"Welcome","avancedb":"Welcome","uuid":"a2db86472466bcd02e84ac05a6c86185","version":"1.6.1","vendor":{"version":"0.0.1","name":"Ripcord Software"}})");
    return true;
}

bool RestServer::GetUuids(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    int count = 1;
    
    if (request->getQueryString().IsKey("count")) {
        auto countParam = request->getQueryString().getValue("count");
        if (countParam.length() > 0) {
            count = boost::lexical_cast<int>(countParam);
        }
    }
    
    std::stringstream stream;
    stream << std::hex << std::setfill('0') << "{\"uuids\":[";
    
    boost::uuids::random_generator gen;
    for (int i = 0; i < count; ++i) {                
        stream << (i > 0 ? "," : "") << "\"";

        auto uuid = gen();
        for (auto iter = uuid.begin(); iter != uuid.end(); ++iter) {
            stream << std::setw(2) << static_cast<unsigned>(*iter);
        }
        
        stream << "\"";
    }
    
    stream << "]}";
    
    response->setContentType("application/javascript").Send(stream.str());    
    return true;
}

bool RestServer::HeadDatabase(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    auto iter = args.find("db");

    bool found = iter != args.cend() && databases_.IsDatabase(iter->second);
    if (found) {
        response->setContentType("application/json").Send();            
    }
    
    return found;            
}

bool RestServer::GetDatabase(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    auto argsIter = args.find("db");
    
    bool found = argsIter != args.cend();
    if (found) {
        auto db = databases_.GetDatabase(argsIter->second);
        
        found = !!db;
        if (found) {
            JsonStream stream;
            stream.Append("committed_update_seq", db->CommitedUpdateSequence());
            stream.Append("compact_running", false);
            stream.Append("data_size", db->DataSize());
            stream.Append("db_name", argsIter->second);
            stream.Append("disk_format_version", 6);
            stream.Append("disk_size", db->DiskSize());
            stream.Append("doc_count", db->DocCount());
            stream.Append("doc_del_count", db->DocDelCount());
            stream.Append("instance_start_time", db->InstanceStartTime());
            stream.Append("purge_seq", db->PurgeSequence());
            stream.Append("update_seq", db->UpdateSequence());
            
            response->setContentType("application/json").Send(stream.Flush());
        } else {
            throw MissingDatabase();
        }
    }
    
    return found;
}

bool RestServer::PutDatabase(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    bool created = false;
    auto argsIter = args.find("db");
    
    if (argsIter != args.cend()) {
        auto name = argsIter->second;
        
        if (std::strlen(name) <= 0 || name[0] == '_') {
            throw InvalidDatabaseName();
        }
        
        if (databases_.IsDatabase(name)) {
            throw DatabaseAlreadyExists();
        }

        created = databases_.AddDatabase(name);
        
        if (created) {
            response->setStatusCode(201).setContentType("application/json").Send(R"({"ok":true})");
        }
    }
    
    return created;
}

bool RestServer::GetDocument(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    bool gotDoc = false;
    auto db = GetDatabase(args);
    if (!!db) {
        auto id = GetParameter("id", args);
        
        auto doc = db->GetDocument(id);
        auto obj = doc->getObject();
        
        auto rev = doc->getRev();
        auto json = ScriptObjectStream::Serialize(obj);
        response->setStatusCode(200).setContentType("application/json").setETag(rev).Send(json);
        
        gotDoc = true;
    }
    
    return gotDoc;
}

bool RestServer::PutDocument(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    bool created = false;
    auto db = GetDatabase(args);
    if (!!db) {
        auto id = GetParameter("id", args);
        auto obj = GetJsonBody(request);        

        if (!!obj) {
            auto doc = db->SetDocument(id, obj);
            
            auto rev = doc->getRev();
            
            JsonStream stream;
            stream.Append("ok", "true");
            stream.Append("id", doc->getId());
            stream.Append("rev", rev);

            response->setStatusCode(201).setContentType("application/json").setETag(rev).Send(stream.Flush());

            created = true;
        }
    }
    return created;
}

bool RestServer::DeleteDocument(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    bool deleted = false;
    auto db = GetDatabase(args);
    if (!!db) {
        auto id = GetParameter("id", args);
        
        auto doc = db->DeleteDocument(id);
        
        auto rev = doc->getRev();
            
        JsonStream stream;
        stream.Append("ok", "true");
        stream.Append("id", doc->getId());
        stream.Append("rev", rev);

        response->setStatusCode(200).setContentType("application/json").setETag(rev).Send(stream.Flush());
        
        deleted = true;        
    }
    
    return deleted;
}

bool RestServer::HeadDocument(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    bool gotHead = false;
    auto db = GetDatabase(args);
    if (!!db) {
        auto id = GetParameter("id", args);
        
        auto doc = db->GetDocument(id);        
        auto rev = doc->getRev();
        
        response->setStatusCode(200).setContentType("application/json").setETag(rev).Send();
        
        gotHead = true;
    }
    
    return gotHead;
}

bool RestServer::DeleteDatabase(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    bool deleted = false;
    auto argsIter = args.find("db");
    
    if (argsIter != args.cend()) {
        auto name = argsIter->second;
        
        if (std::strlen(name) <= 0 || name[0] == '_') {
            throw InvalidDatabaseName();
        }
        
        deleted = databases_.RemoveDatabase(argsIter->second);
        
        if (deleted) {
            response->setContentType("application/json").Send(R"({"ok":true})");
        } else {
            throw MissingDatabase();
        }
    }
    
    return deleted;
}

bool RestServer::GetDatabaseAllDocs(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs& args, rs::httpserver::response_ptr response) {
    response->setContentType("application/json").Send(R"({"offset":0,"rows":[],"total_rows":0})");
}

bool RestServer::GetConfigQueryServers(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    response->setContentType("application/json").Send(R"({"javascript":"libjsapi"})");
}

bool RestServer::GetConfigNativeQueryServers(rs::httpserver::request_ptr request, const rs::httpserver::RequestRouter::CallbackArgs&, rs::httpserver::response_ptr response) {
    response->setContentType("application/json").Send("{}");
}

database_ptr RestServer::GetDatabase(const rs::httpserver::RequestRouter::CallbackArgs& args) {
    database_ptr db;
    auto argsIter = args.find("db");
    if (argsIter != args.cend()) {
        auto dbName = argsIter->second;
        
        db = databases_.GetDatabase(dbName);
    }
    return db;
}

const char* RestServer::GetDatabaseName(const rs::httpserver::RequestRouter::CallbackArgs& args) {
    const char* dbName = nullptr;
    auto argsIter = args.find("db");
    if (argsIter != args.cend()) {
        dbName = argsIter->second;
    }
    return dbName;
}

const char* RestServer::GetParameter(const char* param, const rs::httpserver::RequestRouter::CallbackArgs& args) {
    auto argsIter = args.find(param);
    if (argsIter == args.cend()) {
        throw InvalidJson();
    }
    
    return argsIter->second;
}

rs::scriptobject::ScriptObjectPtr RestServer::GetJsonBody(rs::httpserver::request_ptr request) {
    if (request->HasBody()) {
        if (request->getContentType().find("application/json") == std::string::npos) {
            throw InvalidJson();
        }

        auto& requestStream = request->getRequestStream();
        auto requestLength = request->getContentLength();
        
        std::vector<rs::httpserver::RequestStream::byte> buffer(requestLength);
        
        decltype(requestLength) offset = 0;
        while (offset < requestLength) {
            auto remaining = requestLength - offset;
            auto bytesRead = requestStream.Read(&buffer[offset], 0, remaining, false);
            
            if (bytesRead <= 0) {
                throw InvalidJson();
            }
            
            offset += bytesRead;
        }
        
        auto json = reinterpret_cast<char*>(buffer.data());
        
        try {
            rs::scriptobject::ScriptObjectJsonSource source(json);        
            return rs::scriptobject::ScriptObjectFactory::CreateObject(source);
        } catch (const std::exception&) {
            throw InvalidJson();
        }
    } else {
        return rs::scriptobject::ScriptObjectPtr(nullptr);
    }
}