#define _CRT_SECURE_NO_WARNINGS
#define BOOST_FILESYSTEM_NO_DEPRECATED
// upload_file_to_yadisk.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/locale.hpp>

#include <cpprest/http_client.h>
#include <cpprest/fileStream.h>
#include <cpprest/uri.h>
#include <cpprest/json.h>

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cinttypes>
#include <string>
#include <regex>

#include <ctime>
#include <cstdlib> // defines putenv in POSIX
#include <iomanip>

#include "sqlite3.h"
#include "settings.h"
#include "dbSqlite3.h"
#include "encryptFile.h"
#include "upload_files_to_Dropbox.h"


namespace logging = boost::log;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace keywords = boost::log::keywords;
namespace trivial = boost::log::trivial;

using namespace utility;
using namespace web;
using namespace web::http;
using namespace web::http::client;
using namespace concurrency::streams;
using namespace boost::locale;
using namespace boost::filesystem;
using namespace std;
using namespace sqlite;

using namespace src;
using namespace sinks;
using namespace keywords;
using namespace trivial;


struct FileInfo
{
    string_t name;
    string_t id;
    string_t client_modified;
    size_t size;
};

struct SpaceAllocationInfo
{
    long long used;
    long long allocated;
};
const  long chunckSize =  150 * 1024 * 1024;
string_t accessToken;
string_t destPath;
string_t sourcePath;
string_t password;
int iter;
std::string logfile;
std::string dbfile;


src::severity_logger< logging::trivial::severity_level > lg;
void init()
{
    logging::add_file_log
    (
        keywords::file_name = logfile + "_%N.log",
        keywords::rotation_size = 10 * 1024 * 1024,
        keywords::time_based_rotation = sinks::file::rotation_at_time_point(0, 0, 0),
        keywords::format = "[%TimeStamp%]: %Message%"
    );

    logging::core::get()->set_filter
    (
        logging::trivial::severity >= logging::trivial::info
    );
    
}
SpaceAllocationInfo spaceUsageInfo()
{
    SpaceAllocationInfo spaceAllocation;
    auto spaceUsageTask = pplx::create_task([]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        auto response = http_client(U("https://api.dropboxapi.com/2/users/get_space_usage"))
            .request(request);
        return response;
        }).then([&spaceAllocation](http_response response) {

            if (response.status_code() != web::http::status_codes::OK) {
                using namespace trivial;
                BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                throw std::runtime_error("Returned " + std::to_string(response.status_code()));
            }
            // Convert the response body to JSON object.
            return response.extract_json();
            })

            // Parse the user details.
                .then([&spaceAllocation](json::value jsonObject) {
                spaceAllocation.used = jsonObject[U("used")].as_number().to_int64();
                spaceAllocation.allocated = jsonObject[U("allocation")][U("allocated")].as_number().to_int64();

                    });

            try {
                spaceUsageTask.wait();
            }
            catch (const std::exception& e) {
                printf("Error exception:%s\n", e.what());
            }
            return spaceAllocation;

}


void getCurrentAccount()
{
    auto postJson = pplx::create_task([]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        //        request.headers().add(U("Content-Type"), U("application/json"));

        auto response = http_client(U("https://api.dropboxapi.com/2/users/get_current_account"))
            .request(request);
        return response;
        }).then([](http_response response) {

            if (response.status_code() != web::http::status_codes::OK) {
                using namespace trivial;
                BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());

                throw std::runtime_error("Returned " + std::to_string(response.status_code()));
            }

            // Convert the response body to JSON object.

            return response.extract_json();



            })

            // Parse the user details.
                .then([](json::value jsonObject) {
                    });

            postJson.wait();


}
void  getFilesList(std::shared_ptr< std::vector<FileInfo>> pfilesList, string_t path, string_t cursor)
{

    if (!pfilesList->empty())
    {
        pfilesList->clear();
    }
    if (cursor.empty()) {
        auto task = pplx::create_task([pfilesList, &path, &cursor]() {
            web::http::http_request request(methods::POST);
            request.headers().add(U("Authorization"), accessToken);
            request.headers().add(U("Content-Type"), U("application/json"));
            json::value parameters;
            parameters[U("path")] = json::value::string(path, true);
            parameters[U("recursive")] = json::value::boolean(true);
            //parameters[U("include_media_info")] = json::value::boolean(false);
            //parameters[U("include_deleted")] = json::value::boolean(false);
            //parameters[U("include_has_explicit_shared_members")] = json::value::boolean(false);
            //parameters[U("include_mounted_folders")] = json::value::boolean(true);
            //parameters[U("include_non_downloadable_files")] = json::value::boolean(true);
            parameters[U("limit")] = json::value::number(1000);
            request.set_body(parameters);
            auto response = http_client(U("https://api.dropboxapi.com/2/files/list_folder"))
                .request(request);
            return response;
            }).then([pfilesList, &path, &cursor](http_response response) {

                if (response.status_code() != web::http::status_codes::OK) {
                    using namespace trivial;
                    BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());

                    throw std::runtime_error("Returned " + std::to_string(response.status_code()));
                }

                // Convert the response body to JSON object.

                return response.extract_json();



                })

                // Parse the user details.
                    .then([pfilesList, &path, &cursor](json::value jsonObject) {

                    //std::wcout << jsonObject << std::endl;
                    for (int i = 0; !(jsonObject[U("entries")][i].is_null()); i++)

                    {
                        if (jsonObject[U("entries")][i][U(".tag")] == json::value::string(U("file")))
                        {
                            FileInfo* info = new FileInfo();
                            info->name = jsonObject[U("entries")][i][U("name")].as_string();
                            info->id = jsonObject[U("entries")][i][U("id")].as_string();
                            info->client_modified = jsonObject[U("entries")][i][U("client_modified")].as_string();
                            info->size = jsonObject[U("entries")][i][U("size")].as_integer();
                            ;
                            pfilesList->push_back(*info);
                        }
                    };
                    if (jsonObject[U("has_more")] == json::value::boolean(true))
                    {
                        cursor = jsonObject[U("cursor")].as_string();
                        getFilesList(pfilesList, path, cursor);
                    }

                        });
                task.wait();
    }
    else
    {
        auto task = pplx::create_task([pfilesList, &path, &cursor]() {
            web::http::http_request request(methods::POST);
            request.headers().add(U("Authorization"), accessToken);
            request.headers().add(U("Content-Type"), U("application/json"));
            json::value parameters;
            //parameters[U("path")] = json::value::string(U(""));
            //parameters[U("recursive")] = json::value::boolean(true);
            //parameters[U("include_media_info")] = json::value::boolean(false);
            //parameters[U("include_deleted")] = json::value::boolean(false);
            //parameters[U("include_has_explicit_shared_members")] = json::value::boolean(false);
            //parameters[U("include_mounted_folders")] = json::value::boolean(true);
            //parameters[U("include_non_downloadable_files")] = json::value::boolean(true);
            parameters[U("cursor")] = json::value::string(cursor);
            request.set_body(parameters);

            auto response = http_client(U("https://api.dropboxapi.com/2/files/list_folder/continue"))
                .request(request);
            return response;
            }).then([pfilesList, &path, &cursor](http_response response) {

                if (response.status_code() != web::http::status_codes::OK) {
                    using namespace trivial;
                    BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                    throw std::runtime_error("Returned " + std::to_string(response.status_code()));
                }

                // Convert the response body to JSON object.

                return response.extract_json();



                })

                // Parse the user details.
                    .then([pfilesList, &path, &cursor](json::value jsonObject) {

                    // std::wcout << jsonObject << std::endl;

                    for (int i = 0; !(jsonObject[U("entries")][i].is_null()); i++)

                    {
                        if (jsonObject[U("entries")][i][U(".tag")] == json::value::string(U("file")))
                        {
                            FileInfo* info = new FileInfo();
                            info->name = jsonObject[U("entries")][i][U("name")].as_string();
                            info->id = jsonObject[U("entries")][i][U("id")].as_string();
                            info->client_modified = jsonObject[U("entries")][i][U("client_modified")].as_string();
                            info->size = jsonObject[U("entries")][i][U("size")].as_integer();
                            ;
                            pfilesList->push_back(*info);
                        }
                    };
                    if (jsonObject[U("has_more")] == json::value::boolean(true))
                    {
                        cursor = jsonObject[U("cursor")].as_string();

                        getFilesList(pfilesList, path, cursor);
                    }

                        });
                task.wait();


    }
}

void listFolder()
{
    std::vector<FileInfo> filesList;
    std::shared_ptr< std::vector<FileInfo>> pfilesList = std::make_shared< std::vector<FileInfo>>(filesList);
    string_t cursor;
    try {
        getFilesList(pfilesList, U("/Homework/math"), cursor);
        for (auto e : *pfilesList)
        {
            std::wcout << e.name << " " << e.id << " " << e.size << " " << e.client_modified << std::endl;
        }
    }
    catch (const std::exception& e) {
        printf("Error exception:%s\n", e.what());
    }

}

void test()
{

    json::value js;
    js[U("file_requests")][0][U("id")] = json::value::string(U("rxwMPvK3ATTa0VxOJu5T10"));
    js[U("file_requests")][0][U("title")] = json::value::string(U("title 10"));
    js[U("file_requests")][1][U("id")] = json::value::string(U("rxwMPvK3ATTa0VxOJu5T1"));
    js[U("file_requests")][2][U("id")] = json::value::string(U("rxwMPvK3ATTa0VxOJu5T2"));
    js[U("file_requests")][3][U("id")] = json::value::string(U("rxwMPvK3ATTa0VxOJu5T3"));
    js[U("file_requests")][4][U("id")] = json::value::string(U("rxwMPvK3ATTa0VxOJu5T4"));
    std::wcout << js << std::endl;
    for (int i = 0; i < 5; i++) { std::wcout << js[U("file_requests")][i] << std::endl; };

    int i;
    std::cin >> i;
}

void uploadLargeFile(string_t session, string_t session_id, size_t offset, size_t length, std::shared_ptr<Concurrency::streams::istream> fileStream, string_t fileName, shared_ptr<web::http::status_code> pstatus_code)
{
    if (session.empty())
    {
        auto sessionStart = pplx::create_task([fileStream, &offset, &length, &session_id, &fileName,&pstatus_code]() {
            fileStream->seek(0, std::ios::end);
            length = static_cast<size_t>(fileStream->tell());
            offset = 0;
            session_id = U("");
            json::value jsonObject;
            jsonObject[U("close")] = json::value::boolean(false);
            web::http::http_request request(methods::POST);
            request.headers().add(U("Authorization"), accessToken);
            request.headers().add(U("Dropbox-API-Arg"), jsonObject);
            request.headers().add(U("Content-Type"), U("application/octet-stream"));
            fileStream->seek(0, std::ios::beg);
            request.set_body(*fileStream, chunckSize);
            auto response = http_client(U("https://content.dropboxapi.com/2/files/upload_session/start"))
                .request(request);
            return response;
            }).then([fileStream, &offset, &length, &session_id, &fileName, &pstatus_code](http_response response) {
                *pstatus_code = response.status_code();
                if (*pstatus_code != web::http::status_codes::OK) {
                    using namespace trivial;
                    BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                    fileStream->close();
                    throw std::runtime_error("Returned from upload_session/start " + std::to_string(response.status_code()));
                }

                return response.extract_json(); })
                .then([fileStream, &offset, &length, &session_id, &fileName, &pstatus_code](json::value jsonObject) {
                    offset = chunckSize;
                    session_id = jsonObject[U("session_id")].as_string();
                    if ((length - offset) > chunckSize)
                    {
                        uploadLargeFile(U("append"), session_id, offset, length, fileStream, fileName, pstatus_code);

                    }
                    else
                    {
                        uploadLargeFile(U("finish"), session_id, offset, length, fileStream, fileName, pstatus_code);

                    }

                    });
                sessionStart.wait();
    }
    else

        if (session == U("append"))
        {
            auto sessionAppend = pplx::create_task([fileStream, &offset, &length, &session_id, &fileName]() {
                web::http::http_request request(methods::POST);
                request.headers().add(U("Authorization"), accessToken);
                json::value parameters;
                parameters[U("cursor")][U("session_id")] = json::value::string(session_id);
                parameters[U("cursor")][U("offset")] = json::value::number(offset);
                parameters[U("close")] = json::value::boolean(false);
                request.headers().add(U("Dropbox-API-Arg"), parameters);
                request.headers().add(U("Content-Type"), U("application/octet-stream"));
                request.set_body(*fileStream, chunckSize);
                return http_client(U("https://content.dropboxapi.com/2/files/upload_session/append_v2"))
                    .request(request);

                }).then([fileStream, &offset, &length, &session_id, &fileName,&pstatus_code](http_response response) {
                    *pstatus_code = response.status_code();
                    if (*pstatus_code != web::http::status_codes::OK) {
                        using namespace trivial;
                        BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                        fileStream->close();
                        throw std::runtime_error("Returned from upload_session/append  " + std::to_string(response.status_code()));
                    }

                    // Convert the response body to JSON object.
                    offset += chunckSize;
                    return response.extract_json();

                    }).then([fileStream, &offset, &length, &session_id, &fileName, &pstatus_code](json::value jsonObject) {
                        if ((length - offset) > chunckSize)
                        {
                            uploadLargeFile(U("append"), session_id, offset, length, fileStream, fileName, pstatus_code);

                        }
                        else
                        {
                            uploadLargeFile(U("finish"), session_id, offset, length, fileStream, fileName, pstatus_code);

                        }
                        });
                    sessionAppend.wait();


        }    // append;
        else
            if (session == U("finish"))
            {

                auto sessionFinish = pplx::create_task([&fileName, fileStream, &offset, &length, &session_id, &pstatus_code]() {
                    web::http::http_request request(methods::POST);
                    request.headers().add(U("Authorization"), accessToken);
                    json::value parameters;
                    parameters[U("cursor")][U("session_id")] = json::value::string(session_id);
                    parameters[U("cursor")][U("offset")] = json::value::number(offset);
                    parameters[U("commit")][U("path")] = json::value::string(fileName);
                    parameters[U("commit")][U("mode")] = json::value::string(U("overwrite"));
                    parameters[U("commit")][U("autorename")] = json::value::boolean(true);
                    parameters[U("commit")][U("mute")] = json::value::boolean(false);
                    parameters[U("commit")][U("strict_conflict")] = json::value::boolean(false);
                    //request.headers().add(U("Dropbox-API-Arg"), parameters);
                    request.headers().add(U("Content-Type"), U("application/octet-stream"));
                    request.set_body(*fileStream, length - offset);
                    uri_builder builder(U("https://content.dropboxapi.com/2/files/upload_session/finish"));
                    builder.append_query(U("arg"), parameters.serialize());
                    auto response = http_client(builder.to_uri()).request(request);
                    return response;
                    }).then([&fileStream, &pstatus_code](http_response response) {
                        *pstatus_code = response.status_code();
                        fileStream->close();
                        if (*pstatus_code != web::http::status_codes::OK) {
                            using namespace trivial;
                            BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                            BOOST_LOG_SEV(lg, error) << " response:" << response.to_string();
                            throw std::runtime_error("Returned upload_session/finish " + std::to_string(response.status_code()));
                        }

                        // Convert the response body to JSON object.

                        return response.extract_json();
                        })

                        .then([ &pstatus_code](json::value jsonObject) {
                            });
                        sessionFinish.wait();


            } // finish


}

void uploadLT150(size_t length, std::shared_ptr<Concurrency::streams::istream> fileStream, string_t fileName, shared_ptr<web::http::status_code> pstatus_code)
{
    // Content-Length property
    fileStream->seek(0, std::ios::end);
    length = static_cast<size_t>(fileStream->tell());

    fileStream->seek(0, std::ios::beg);
    auto posttask = pplx::create_task([&length, fileStream, &fileName,&pstatus_code]() {
        json::value jsonObject;
        jsonObject[U("path")] = json::value::string(fileName);
        jsonObject[U("mode")] = json::value::string(U("overwrite"));
        jsonObject[U("autorename")] = json::value::boolean(true);
        jsonObject[U("mute")] = json::value::boolean(false);
        jsonObject[U("strict_conflict")] = json::value::boolean(false);
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        request.headers().add(U("Content-Type"), U("application/octet-stream"));
        request.set_body(*fileStream, length);
        uri_builder builder(U("https://content.dropboxapi.com/2/files/upload"));
        builder.append_query(U("arg"), jsonObject.serialize());
        auto response = http_client(builder.to_uri()).request(request);
        return response;
        })
        // Get the response.
            .then([fileStream, &pstatus_code](http_response response) {
            // Check the status code.
            fileStream->close();
            *pstatus_code = response.status_code();
            if (*pstatus_code != web::http::status_codes::OK) {
                using namespace trivial;
                BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                BOOST_LOG_SEV(lg, error) << " response:" << response.to_string();
                throw std::runtime_error("Returned from files/upload " + std::to_string(response.status_code()));
            }
            // Convert the response body to JSON object.
            return response.extract_json();
                })
            // Parse the user details.
                    .then([&pstatus_code](json::value jsonObject) {
                        });
                posttask.wait();

}

void uploadFile(string_t LocalFiletoUpload, string_t destFileName, shared_ptr<web::http::status_code> pstatus_code)
{
    BOOST_LOG_SEV(lg, trivial::info) << "file: source  " << LocalFiletoUpload << " dest "<< destFileName;
   
    std::shared_ptr<Concurrency::streams::istream> fileStream = std::make_shared<Concurrency::streams::istream>();
    size_t offset{ 0 };
    size_t length{ 0 };
    string_t session_id;
    string_t session;
    string_t fileName;

    fileName = destPath + U("/") + destFileName;
    auto filestream = file_stream<uint8_t>::open_istream(LocalFiletoUpload)
        .then([fileName, &fileStream, &offset, &length, session, &session_id,&pstatus_code]( pplx::task<Concurrency::streams::basic_istream<unsigned char>> previousTask) {

        *fileStream = previousTask.get();
        fileStream->seek(0, std::ios::end);
        length = static_cast<size_t>(fileStream->tell());
        SpaceAllocationInfo saInfo = spaceUsageInfo();
        if ((saInfo.allocated - saInfo.used - length) >= 0)
        {

            fileStream->seek(0, std::ios::beg);
            if (length < 150 * 1024 * 1024)
            {
                uploadLT150(length, fileStream, fileName, pstatus_code);
            }
            else
            {
                uploadLargeFile(session, session_id, offset, length, fileStream, fileName, pstatus_code);
            }
        }
            });
    filestream.wait();
    fileStream->close();

   
}

void deletePath(string_t path)
{
    auto taskDeletePath = pplx::create_task([&path]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), U("Bearer rCHrqYDvgjgAAAAAAAAA0oW2g67iyyUS0SNnYBa0rQ1i_RJTX3TfncA6Q4yOg0Oq"));
        request.headers().add(U("Content-Type"), U("application/json"));
        json::value parameters;
        parameters[U("path")] = json::value::string(path, true);
        request.set_body(parameters);
        auto response = http_client(U("https://api.dropboxapi.com/2/files/delete_v2"))
            .request(request);
        return response;
        }).then([](http_response response) {

            if (response.status_code() != web::http::status_codes::OK) {
                using namespace trivial;
                BOOST_LOG_SEV(lg, error) << " Error exception:" << std::to_string(response.status_code());
                throw std::runtime_error("Returned " + std::to_string(response.status_code()));
            }

            // Convert the response body to JSON object.

            return response.extract_json();



            })

            // Parse the user details.
                .then([](json::value jsonObject) {
                // std::wcout << " json=" << jsonObject << std::endl;;

                    });

            taskDeletePath.wait();


}
string_t getOldFile()
{
    std::vector<FileInfo> filesList;
    std::shared_ptr< std::vector<FileInfo>> pfilesList = std::make_shared< std::vector<FileInfo>>(filesList);
    string_t cursor;
    getFilesList(pfilesList, destPath, cursor);
    if (pfilesList->size() > 0)
    {
        std::vector<FileInfo>::iterator element_min = std::min_element(pfilesList->begin(), pfilesList->end(), [](auto a, auto b) { return (a.client_modified < b.client_modified);  });
        return element_min->name;
    }
    else return U("");
}
std::string cp1251_to_utf8(const char* str) {
    using namespace std;
    string res;
    int result_u, result_c;
    result_u = MultiByteToWideChar(1251, 0, str, -1, 0, 0);
    if (!result_u) { return 0; }
    wchar_t* ures = new wchar_t[result_u];
    if (!MultiByteToWideChar(1251, 0, str, -1, ures, result_u)) {
        delete[] ures;
        return 0;
    }
    result_c = WideCharToMultiByte(65001, 0, ures, -1, 0, 0, 0, 0);
    if (!result_c) {
        delete[] ures;
        return 0;
    }
    char* cres = new char[result_c];
    if (!WideCharToMultiByte(65001, 0, ures, -1, cres, result_c, 0, 0)) {
        delete[] cres;
        return 0;
    }
    delete[] ures;
    res.append(cres);
    delete[] cres;
    return res;
}
std::wstring string_to_wstring(std::string str) {
    using namespace std;
    wstring convertedString;
    int requiredSize = MultiByteToWideChar(1251, 0, str.c_str(), -1, 0, 0);
    if (requiredSize > 0) {
        vector<wchar_t> buffer(requiredSize);
        MultiByteToWideChar(1251, 0, str.c_str(), -1, &buffer[0], requiredSize);
        convertedString.assign(buffer.begin(), buffer.end() - 1);
    }
    return convertedString;
}


void remove_old_files(std::vector<path>& filesListToUpload)
{
    sort(filesListToUpload.begin(), filesListToUpload.end(), [](auto a, auto b)->bool { return (boost::filesystem::last_write_time(a) > boost::filesystem::last_write_time(b)); });
    std::time_t max_time_t = boost::filesystem::last_write_time(filesListToUpload[0]);
    auto newEnd = std::remove_if(filesListToUpload.begin(), filesListToUpload.end(), [&max_time_t](auto a)->bool { return (max_time_t - boost::filesystem::last_write_time(a)) > 3 * 24 * 60 * 60; });
    filesListToUpload.erase(newEnd, filesListToUpload.end());
    for (auto p : filesListToUpload)
    {
        BOOST_LOG_SEV(lg, trivial::info) << p << boost::filesystem::last_write_time(p) << std::endl;

    }
}

void remove_uploaded_files(std::vector<path>& filesListToUpload, std::shared_ptr<std::vector<FileInfo>> pfileList)
{
    auto newEnd = remove_if(filesListToUpload.begin(), filesListToUpload.end(), [&pfileList](path p)->bool
        {
            auto Name = p.filename().wstring();
            auto fnd = find_if(pfileList->begin(), pfileList->end(), [&Name](auto fi) ->bool
                {

                    return (fi.name.find(Name) != std::wstring::npos);
                });
            return  fnd != pfileList->end();
        }
    );
    filesListToUpload.erase(newEnd, filesListToUpload.end());
}

//
//
//
void find_folder(string_t destPath, std::shared_ptr<bool>& pFound)
{
    *pFound = false;
    string_t folder = destPath;

    auto task = pplx::create_task([folder, pFound]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        request.headers().add(U("Cache-Control"), U("no-cache"));
        request.headers().add(U("Content-Type"), U("application/json"));
        request.headers().add(U("Accept-Charset"), U("utf-8"));
        json::value parameters;
        parameters[U("path")] = json::value::string(folder,true);
        request.set_body(parameters);
        auto response = http_client(U("https://api.dropboxapi.com/2/files/get_metadata")).request(request);
        return response;
        }).then([folder, pFound](http_response response) {
            //std::wcout << "response.status_code() " << response.status_code() << std::endl;
            switch (response.status_code()) {
            case web::http::status_codes::OK:
                *pFound = true;
                break;
            case web::http::status_codes::NotFound:
                *pFound = false;
                break;
            case web::http::status_codes::Conflict:
                *pFound = false;
                break;
            default:
               throw std::runtime_error("Returned " + std::to_string(response.status_code()));
            };
            return response.extract_json();
            }).then([](json::value jsonObject) {
               // std::wcout << "jsonObject " << jsonObject.serialize() << std::endl;

                });
        task.wait();
}
//
//
//

void make_folder(string_t destPath)
{
    std::wcout << "make_folder " << destPath << std::endl;
    auto create_folder = pplx::create_task([destPath]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        request.headers().add(U("Cache-Control"), U("no-cache"));
        request.headers().add(U("Content-Type"), U("application/json"));
        request.headers().add(U("Accept-Charset"), U("utf-8"));
        json::value parameters;
        parameters[U("path")] = json::value::string(destPath);
        request.set_body(parameters);
        auto response = http_client(U("https://api.dropboxapi.com/2/files/create_folder_v2")).request(request);
        return response;
        }).then([destPath](http_response response) {
            if (response.status_code() != web::http::status_codes::OK)
                throw std::runtime_error("Returned " + std::to_string(response.status_code()));
            return response.extract_json();
            }
        
        ).then([](json::value jsonObject) {
                });
        create_folder.wait();

}
//
//
//
void downloadFile(string_t sourcePath, string_t sourceFile, shared_ptr<web::http::status_code> presult, string_t destFile)
{
        auto downloadTask = pplx::create_task([ presult, sourcePath, sourceFile, destFile]() {
            web::http::http_request request(methods::POST);
            request.headers().add(U("Authorization"), accessToken);
            request.headers().add(U("Cache-Control"), U("no-cache"));
            request.headers().add(U("Content-Type"), U("application/octet-stream; charset=utf-8"));
            request.headers().add(U("Accept-Charset"), U("utf-8"));
            json::value parameters;
            auto path = sourcePath + U("/")+sourceFile;
            parameters[U("path")] = json::value::string(path,true);
            uri_builder builder(U("https://content.dropboxapi.com/2/files/download"));
            builder.append_query(U("arg"), parameters.serialize());

            auto response = http_client(builder.to_uri()).request(request);
            return response;
            }).then([presult, destFile](http_response response) {
                *presult = response.status_code();

                if (response.status_code() != web::http::status_codes::OK) {

                   // std::wcout << response.to_string() << std::endl;
                    throw std::runtime_error("Returned: downloadFile  " + std::to_string(response.status_code()));
                }
                return response.body();
                }).then([destFile](Concurrency::streams::istream is) {

                    try
                    {
                        std::shared_ptr<Concurrency::streams::ostream> fileStream = std::make_shared<Concurrency::streams::ostream>();
                        auto filestreamTask = file_stream<uint8_t>::open_ostream(destFile).then([&fileStream,is](pplx::task<Concurrency::streams::basic_ostream<unsigned char>> previousTask) {
                            *fileStream = previousTask.get();
                            Concurrency::streams::streambuf<uint8_t> rwbuf = fileStream->streambuf();
                            auto readtask = is.read_to_end(rwbuf);
                            readtask.wait();
                            size_t read = readtask.get();
                            fileStream->close();
                            });
                        filestreamTask.wait();
                    }
                    catch (...)
                    {
                        throw;
                    };
                    });

        downloadTask.wait();
     
}



//
void find_file(string_t dest_path, string_t dest_file, std::shared_ptr<bool>& pFound, shared_ptr<size_t> pSize_t)
{
    *pFound = false;
    web::http::status_code status_code;
    auto task = pplx::create_task([dest_file,dest_path, pFound, pSize_t, &status_code]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        request.headers().add(U("Cache-Control"), U("no-cache"));
        request.headers().add(U("Content-Type"), U("application/json"));
        request.headers().add(U("Accept-Charset"), U("utf-8"));
        json::value parameters;
        parameters[U("query")] = json::value::string(dest_file, true);
        parameters[U("options")][U("path")] = json::value::string(dest_path);
        parameters[U("options")][U("filename_only")] = json::value::boolean(true);
        request.set_body(parameters);
        auto uri_bld = uri_builder(U("https://api.dropboxapi.com/2/files/search_v2"));
        auto response = http_client(uri_bld.to_uri()).request(request);
        return response;
        }).then([ pFound, pSize_t, &status_code](http_response response) {
            status_code = response.status_code();
            switch (status_code) {
            case web::http::status_codes::OK:
                *pFound = true;
                break;
            case web::http::status_codes::NotFound:
                *pFound = false;
                break;
            default:
                *pFound = false;
                throw std::runtime_error("find_file: Returned " + std::to_string(response.status_code()));
                break;
            }
            return response.extract_json();
            }
        ).then([pFound, pSize_t, &status_code](web::json::value jsonObject) {
                auto matches = jsonObject[U("matches")].as_array();
                *pFound = (matches.size() > 0);

                *pSize_t = 0;
                if (!*pFound) return;
                try
                {

                    *pSize_t =  jsonObject[U("matches")][0][U("metadata")][U("metadata")][U("size")].as_integer();
                }
                catch (exception e)
                {
                    std::wcout << " find_file " << e.what() << std::endl;
                    throw;
                };


            });
            task.wait();
}



void get_metadata(string_t dest_path, string_t dest_file,std::shared_ptr<bool>&pFound, shared_ptr<string_t> pcontent_hash)
{
    string_t filename = dest_path + U("/") + dest_file;
    *pFound = false;
    web::http::status_code status_code;
    auto task = pplx::create_task([filename, pFound, pcontent_hash, &status_code]() {
        web::http::http_request request(methods::POST);
        request.headers().add(U("Authorization"), accessToken);
        request.headers().add(U("Cache-Control"), U("no-cache"));
        request.headers().add(U("Content-Type"), U("application/json"));
        request.headers().add(U("Accept-Charset"), U("utf-8"));
        json::value parameters;
        parameters[U("path")] = json::value::string(filename, true);
        parameters[U("include_media_info")] = json::value::boolean(false);
        parameters[U("include_deleted")] = json::value::boolean(false);
        parameters[U("include_has_explicit_shared_members")]= json::value::boolean(false);
        request.set_body(parameters);
        auto uri_bld = uri_builder(U("https://api.dropboxapi.com/2/files/get_metadata"));
        auto response = http_client(uri_bld.to_uri()).request(request);
        return response;
        }).then([pFound,pcontent_hash, &status_code](http_response response) {
            status_code = response.status_code();
            switch (status_code) {
            case web::http::status_codes::OK:
                *pFound = true;
                break;
            case web::http::status_codes::NotFound:
                *pFound = false;
                break;
            default:
                *pFound = false;
                throw std::runtime_error("find_file: Returned " + std::to_string(response.status_code()));
                break;
            }
            return response.extract_json();
            }
        ).then([pFound, pcontent_hash, &status_code](web::json::value jsonObject) {

                try
                {

                    *pcontent_hash = jsonObject[U("content_hash")].as_string();
                }
                catch (exception e)
                {
                    std::wcout << " get content_hash " << e.what() << std::endl;
                    throw;
                };


            });
            task.wait();
}

bool compare_hashes(string_t& content_hash, char* local_file_hash, int hash_len)
{
    bool result = false;
    string_t local_hash_st = utility::conversions::to_string_t(local_file_hash);
    result = std::equal(begin(local_hash_st), end(local_hash_st), begin(content_hash), end(content_hash),
        [](auto a, auto b) {return (a == b); });
    return result;
}


//openssl enc -aes-256-cbc -pbkdf2 -d -in src_filename -out dst_filename -salt  -md SHA256 -iter 55608

int main(int argc, char** argv)
{
    setlocale(LC_ALL, "russian");
    // Create and install global locale
    std::locale::global(boost::locale::generator().generate(""));  //ru.RU.UTF-8
     //Make boost.filesystem use it
    boost::filesystem::path::imbue(std::locale());
    try
    {
        path application(argv[0]);
        string app = application.replace_extension(".xml").string();
        std::wcout << app << std::endl;
        std::map<std::string, std::string> m = settings::getSettings(app);
        accessToken = utility::conversions::to_string_t(m["accessToken"]);
        sourcePath = utility::conversions::to_string_t(m["sourcePath"]);
        destPath = utility::conversions::to_string_t(m["destPath"]);
        password = utility::conversions::to_string_t(m["password"]);
        logfile = m["logfile"];
        iter = std::stoi(m["iter"]);
        dbfile = m["sqlite"];

    }
    catch (const std::exception& e)
    {
        std::wcout << "exception "<<e.what() << std::endl;
        return 1;
    }

    init();
    logging::add_common_attributes();

    if (argc == 2)
    {

        path enc_file(argv[1]);

        if (!exists(enc_file)) {
            std::wcout << "file not found" << std::endl;
            return 0;
        }
        std::wcout << U("descrypting file ") << enc_file.filename().wstring() << std::endl;

        shared_ptr<char*> ppassword = make_shared<char*>(new char[password.length() + 1]);
        strcpy(*ppassword, utility::conversions::to_utf8string(password).c_str());

        try
        {
            auto des_file = gzipEncrypt::descrypt_path(enc_file, *ppassword, iter);
        }
        catch (exception& e)
        {
            std::cout << U("error while descrypting file ") << enc_file << ":" << e.what() << std::endl;
        }

        return 0;
    }

    
    BOOST_LOG_SEV(lg, trivial::info) << "uploading files from  " << sourcePath << " to " << destPath<<" \n";
    path source_path(sourcePath);
    shared_ptr<bool> pFound = make_shared<bool>();
    *pFound = false;
    try
    {
        find_folder(destPath, pFound);
        if (*pFound == false)
        {
            BOOST_LOG_SEV(lg, trivial::info) << "create folder  " << destPath << "\n";
            make_folder(destPath);
        }
        else
        {
            shared_ptr<web::http::status_code> presult = make_shared< web::http::status_code>();
            path catalog(dbfile);
            string_t  sourceFile = catalog.filename().wstring();
            string_t destFile = catalog.wstring();
            try 
            {
                shared_ptr<bool> pfound = make_shared<bool>();
                shared_ptr<size_t> pfsize = make_shared<size_t>();
                *pfound = false;
                find_file(destPath, sourceFile, pfound, pfsize);
                if (*pfound)
                {
                    BOOST_LOG_SEV(lg, trivial::info) << "download catalog.sqlite  from " << destPath <<" size " << *pfsize<< "\n";
                    downloadFile(destPath, sourceFile, presult, destFile);
                }
            }
            catch (...)
            {
                BOOST_LOG_SEV(lg, trivial::info) << "file  " << sourceFile <<" not found \n";
            }
        }
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_SEV(lg, trivial::info) << "  std::exception " << e.what() << "\n";
        return 1;
    }

    SqliteConnection DB(dbfile);
    std::vector<path> filesListToUpload;
    const std::regex txt_reg(path(logfile).filename().string()+"_\\d+.log");
    for (directory_entry& entry : directory_iterator(source_path))
    {
        path localPath(entry.path());
        const std::string s = localPath.string();
        if (std::regex_search(s, txt_reg))
        {
            BOOST_LOG_SEV(lg, trivial::info) << "skipped log file " << localPath << std::endl;
            continue;
        }
        if (boost::filesystem::is_empty(localPath) || !is_regular_file(localPath) || !exists(localPath))
        {
            BOOST_LOG_SEV(lg, trivial::info)<<"skipped  " << localPath  << std::endl;
            continue;
        }
        if (localPath.extension().string() == ".enc")
        {
            BOOST_LOG_SEV(lg, trivial::info) << "skipped encrypped file " << localPath << std::endl;
            continue;
        }


        if (localPath == path(dbfile))
        {
            BOOST_LOG_SEV(lg, trivial::info) << "skipped catalog file " << localPath << std::endl;
            continue;
        }
       

        if (!DB.fileAllreadyUploaded(localPath))
        {
            filesListToUpload.push_back(localPath);
            BOOST_LOG_SEV(lg, trivial::info)<<" added to list " << localPath << std::endl;
        }
    }

    try
    {
        auto pfileList = make_shared< std::vector<FileInfo>>();
        string_t cursor;
        path source_path(sourcePath);
        for_each(filesListToUpload.begin(), filesListToUpload.end(), [&DB, &pfileList,&cursor](path p) {
            p.make_preferred();
            string_t localFile = p.wstring();
            BOOST_LOG_SEV(lg, trivial::info) << "file to upload " << localFile << std::endl;
            shared_ptr<char*> ppassword = make_shared<char*>(new char[password.length() + 1]);
            strcpy(*ppassword, utility::conversions::to_utf8string(password).c_str());
            path encrypted_path = gzipEncrypt::compressEncryptDeleteFile(p, *ppassword, iter);
            auto size = file_size(encrypted_path);
            SpaceAllocationInfo si = spaceUsageInfo();

            if (si.allocated - size < 0) {
                BOOST_LOG_SEV(lg, trivial::info) << "Allocated space " << si.allocated << " lt file size " << size << std::endl;
                return 0;
            };
            BOOST_LOG_SEV(lg, trivial::info) << "Allocated space " << si.allocated << " used " << si.used <<" avaiblable "<< (si.allocated - si.used) << std::endl;
            if ((si.allocated - si.used -(long long)size) < 0)
            {
                BOOST_LOG_SEV(lg, trivial::info) << "Attemp to delete old files " << std::endl;
                getFilesList(pfileList, destPath, cursor);
                std::sort(pfileList->begin(), pfileList->end(), [](auto a, auto b) { return (a.client_modified < b.client_modified); });

                while (((si.allocated - si.used - (long long) size) < 0) && (pfileList->size() != 0))
                {
                    auto p = *pfileList->begin();
                    if (p.name == utility::conversions::to_string_t(dbfile))
                    {
                        std::wcout << p.name << " " << utility::conversions::to_string_t(dbfile) << std::endl;
                        continue;
                    }
                    deletePath(destPath + U("/") + p.name);
                    pfileList->erase(pfileList->begin());
                    BOOST_LOG_SEV(lg, trivial::info) << "Deleted file " << p.name << "(" << p.size << ")" << std::endl;
                    si = spaceUsageInfo();
                }
            }
            auto destFileName = encrypted_path.filename().wstring();
            BOOST_LOG_SEV(lg, trivial::info) << "src: " << encrypted_path << " dst: " << destFileName<<"\n";
            shared_ptr<web::http::status_code> pstatus_code = make_shared< web::http::status_code>();
            uploadFile(encrypted_path.wstring(), destFileName,pstatus_code);
            BOOST_LOG_SEV(lg, trivial::info) << "upload file "<< encrypted_path.wstring() << "pstatus_code  " << *pstatus_code << std::endl;
            //calculate file hash
            char* hash = new char[65];
            memset(hash, 0, 65);
            int hash_len = 65;
            try
            {
                gzipEncrypt::compute_dropbox_hash(encrypted_path, hash, hash_len);
                auto file_status = boost::filesystem::status(encrypted_path);
                boost::filesystem::remove(encrypted_path);
            }
            catch (boost::filesystem::filesystem_error e)
            {
                BOOST_LOG_SEV(lg, trivial::error) << "remove : filesystem_error   " << e.what() << std::endl;
            }
            catch (...)
            {
                BOOST_LOG_SEV(lg, trivial::error) << "error removing file    " << encrypted_path.string() << std::endl;

            }
            shared_ptr<string_t> pcontent_hash = make_shared<string_t>();
            shared_ptr<bool> pfound = make_shared<bool>();
            get_metadata(destPath, destFileName, pfound, pcontent_hash);
            if (compare_hashes(*pcontent_hash, hash, hash_len))
            {

                shared_ptr<size_t> pfsize = make_shared<size_t>();
                *pfsize = file_size(p);
                std::time_t t = creation_time(p);

                BOOST_LOG_SEV(lg, trivial::info) << "file uploaded " << localFile << " size " << *pfsize << " creation_time " << std::put_time(std::localtime(&t), "%c %Z") << " content hash " << *pcontent_hash << std::endl;
                DB.insertRow(p);
            }
            else
                deletePath(destFileName);
            }
        );  //for

        DB.close_Connection();
        path db(dbfile);
        shared_ptr<web::http::status_code> pstatus_code = make_shared< web::http::status_code>();
        BOOST_LOG_SEV(lg, trivial::info) << "upload catalog  " << db.wstring()<<" size "<< file_size(db) << std::endl;
        uploadFile(db.wstring(), db.filename().wstring(), pstatus_code);
    }
    catch (exception e)
    {
        BOOST_LOG_SEV(lg, trivial::info) << " exception " << e.what()  << std::endl;
    }
}

