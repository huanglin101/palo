// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/broker_reader.h"

#include <sstream>

#include "common/logging.h"
#include "gen_cpp/PaloBrokerService_types.h"
#include "gen_cpp/TPaloBrokerService.h"
#include "runtime/broker_mgr.h"
#include "runtime/client_cache.h"
#include "runtime/exec_env.h"
#include "runtime/runtime_state.h"
#include "util/thrift_util.h"

namespace palo {

// Broker

BrokerReader::BrokerReader(
        RuntimeState* state,
        const std::vector<TNetworkAddress>& broker_addresses,
        const std::map<std::string, std::string>& properties,
        const std::string& path,
        int64_t start_offset) :
            _state(state),
            _addresses(broker_addresses),
            _properties(properties),
            _path(path),
            _cur_offset(start_offset),
            _is_fd_valid(false),
            _eof(false),
            _addr_idx(0) {
}

BrokerReader::~BrokerReader() {
    close();
}

#ifdef BE_TEST
inline BrokerServiceClientCache* client_cache(RuntimeState* state) {
    static BrokerServiceClientCache s_client_cache;
    return &s_client_cache;
}

inline const std::string& client_id(RuntimeState* state, const TNetworkAddress& addr) {
    static std::string s_client_id = "palo_unit_test";
    return s_client_id;
}
#else
inline BrokerServiceClientCache* client_cache(RuntimeState* state) {
    return state->exec_env()->broker_client_cache();
}

inline const std::string& client_id(RuntimeState* state, const TNetworkAddress& addr) {
    return state->exec_env()->broker_mgr()->get_client_id(addr);
}
#endif

Status BrokerReader::open() {
    TBrokerOpenReaderRequest request;

    const TNetworkAddress& broker_addr = _addresses[_addr_idx];
    request.__set_version(TBrokerVersion::VERSION_ONE);
    request.__set_path(_path);
    request.__set_startOffset(_cur_offset);
    request.__set_clientId(client_id(_state, broker_addr));
    request.__set_properties(_properties);

    TBrokerOpenReaderResponse response;
    try {
        Status status;
        // 500ms is enough
        BrokerServiceConnection client(client_cache(_state), broker_addr, 500, &status);
        if (!status.ok()) {
            LOG(WARNING) << "Create broker client failed. broker=" << broker_addr
                << ", status=" << status.get_error_msg();
            return status;
        }

        try {
            client->openReader(response, request);
        } catch (apache::thrift::transport::TTransportException& e) {
            RETURN_IF_ERROR(client.reopen());
            client->openReader(response, request);
        }
    } catch (apache::thrift::TException& e) {
        std::stringstream ss;
        ss << "Open broker reader failed, broker:" << broker_addr << " failed:" << e.what();
        LOG(WARNING) << ss.str();
        return Status(TStatusCode::THRIFT_RPC_ERROR, ss.str(), false);
    }

    if (response.opStatus.statusCode != TBrokerOperationStatusCode::OK) {
        std::stringstream ss;
        ss << "Open broker reader failed, broker:" << broker_addr 
            << " failed:" << response.opStatus.message;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    _fd = response.fd;
    _is_fd_valid = true;
    return Status::OK;
}

Status BrokerReader::read(uint8_t* buf, size_t* buf_len, bool* eof) {
    if (_eof) {
        *eof = true;
        return Status::OK;
    }
    
    const TNetworkAddress& broker_addr = _addresses[_addr_idx];
    TBrokerPReadRequest request;
    request.__set_version(TBrokerVersion::VERSION_ONE);
    request.__set_fd(_fd);
    request.__set_offset(_cur_offset);
    request.__set_length(*buf_len);

    TBrokerReadResponse response;
    try {
        Status status;
        // 500ms is enough
        BrokerServiceConnection client(client_cache(_state), broker_addr, 500, &status);
        if (!status.ok()) {
            LOG(WARNING) << "Create broker client failed. broker=" << broker_addr
                << ", status=" << status.get_error_msg();
            return status;
        }

        try {
            client->pread(response, request);
        } catch (apache::thrift::transport::TTransportException& e) {
            RETURN_IF_ERROR(client.reopen());
            client->pread(response, request);
        }
    } catch (apache::thrift::TException& e) {
        std::stringstream ss;
        ss << "Read from broker failed, broker:" << broker_addr << " failed:" << e.what();
        LOG(WARNING) << ss.str();
        return Status(TStatusCode::THRIFT_RPC_ERROR, ss.str(), false);
    }

    if (response.opStatus.statusCode == TBrokerOperationStatusCode::END_OF_FILE) {
        // read the end of broker's file
        *eof = _eof = true;
        return Status::OK;
    } else if (response.opStatus.statusCode != TBrokerOperationStatusCode::OK) {
        std::stringstream ss;
        ss << "Read from broker failed, broker:" << broker_addr 
            << " failed:" << response.opStatus.message;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    *buf_len = response.data.size();
    memcpy(buf, response.data.data(), *buf_len);
    _cur_offset += *buf_len; 
    *eof = false;

    return Status::OK;
}

void BrokerReader::close() {
    if (!_is_fd_valid) {
        return;
    }
    TBrokerCloseReaderRequest request;

    request.__set_version(TBrokerVersion::VERSION_ONE);
    request.__set_fd(_fd);

    const TNetworkAddress& broker_addr = _addresses[_addr_idx];
    TBrokerOperationStatus response;
    try {
        Status status;
        // 500ms is enough
        BrokerServiceConnection client(client_cache(_state), broker_addr, 500, &status);
        if (!status.ok()) {
            LOG(WARNING) << "Create broker client failed. broker=" << broker_addr
                << ", status=" << status.get_error_msg();
            return;
        }

        try {
            client->closeReader(response, request);
        } catch (apache::thrift::transport::TTransportException& e) {
            status = client.reopen();
            if (!status.ok()) {
                LOG(WARNING) << "Close broker reader failed. broker=" << broker_addr
                    << ", status=" << status.get_error_msg();
                return;
            }
            client->closeReader(response, request);
        }
    } catch (apache::thrift::TException& e) {
        LOG(WARNING) << "Close broker reader failed, broker:" << broker_addr
            << " failed:" << e.what();
        return;
    }

    if (response.statusCode != TBrokerOperationStatusCode::OK) {
        LOG(WARNING) << "Open broker reader failed, broker:" << broker_addr 
            << " failed:" << response.message;
        return;
    }
    _is_fd_valid = false;
}

}
