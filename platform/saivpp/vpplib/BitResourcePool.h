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