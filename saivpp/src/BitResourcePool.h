/*
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
#include <cstdint>
#include <stdexcept>
namespace saivpp
{
    class BitResourcePool {
        public:
            BitResourcePool(uint16_t size, uint32_t base) : resource_size(size), base_index(base){
                if (size > MAX_RESOURCE_SIZE_BYTES * 8) {
                    throw std::invalid_argument("Resource size exceeds maximum size");
                }
            }
            ~BitResourcePool() = default;
            int alloc() {
                for (uint16_t i = 0; i < resource_size; i++) {
                    if ((resource_bitmap[i / 8] & (1 << (i % 8))) == 0) {
                        resource_bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
                        return base_index + i;
                    }
                }
                return -1;
            }
            void free(uint32_t index) {
                if (index >= resource_size + base_index || index < base_index) {
                    throw std::invalid_argument("Invalid index");
                }                
                index -= base_index;
                resource_bitmap[index / 8] &=  (uint8_t)(~(1 << (index % 8)));
            }
        private:
            static const int MAX_RESOURCE_SIZE_BYTES = 16 * 1024;
            uint8_t resource_bitmap[MAX_RESOURCE_SIZE_BYTES] = {0};
            uint16_t resource_size;
            uint32_t base_index;
    };
};