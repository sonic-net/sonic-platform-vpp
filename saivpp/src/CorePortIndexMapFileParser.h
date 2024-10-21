/*
 * Copyright 2016 Microsoft, Inc.
 * Modifications copyright (c) 2023 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "CorePortIndexMapContainer.h"

#include <vector>
#include <memory>

namespace saivpp
{
    class CorePortIndexMapFileParser
    {
        private:

            CorePortIndexMapFileParser() = delete;
            ~CorePortIndexMapFileParser() = delete;

        public:

            static std::shared_ptr<CorePortIndexMapContainer> parseCorePortIndexMapFile(
                    _In_ const char* file);

            static std::shared_ptr<CorePortIndexMapContainer> parseCorePortIndexMapFile(
                    _In_ const std::string& file);

            static bool isInterfaceNameValid(
                    _In_ const std::string& name);

        private:

            static void parseLineWithIndex(
                    _In_ std::shared_ptr<CorePortIndexMapContainer> container,
                    _In_ const std::vector<std::string>& tokens);

            static void parseLineWithNoIndex(
                    _In_ std::shared_ptr<CorePortIndexMapContainer> container,
                    _In_ const std::vector<std::string>& tokens);

            static void parse(
                    _In_ std::shared_ptr<CorePortIndexMapContainer> container,
                    _In_ uint32_t switchIndex,
                    _In_ const std::string& ifname,
                    _In_ const std::string& slanes);
    };
}
