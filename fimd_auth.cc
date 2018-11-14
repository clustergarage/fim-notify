/**
 * MIT License
 * 
 * Copyright (c) 2018 ClusterGarage
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "fimd_auth.h"

#include <string>

#include <grpc/grpc.h>
#include <grpc++/grpc++.h>

namespace fimd {
grpc::Status FimdAuthMetadataProcessor::Process(const grpc::AuthMetadataProcessor::InputMetadata &authMetadata, grpc::AuthContext *context,
    grpc::AuthMetadataProcessor::OutputMetadata *consumedAuthMetadata, grpc::AuthMetadataProcessor::OutputMetadata *responseMetadata) {

    for (auto it = authMetadata.begin(); it != authMetadata.end(); ++it) {
        std::string key(it->first.begin(), it->first.end());
        std::string val(it->second.begin(), it->second.end());
    }
    return grpc::Status::OK;
}
} // namespace fimd