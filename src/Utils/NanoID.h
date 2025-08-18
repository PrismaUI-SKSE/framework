#include <random>
#include <cstddef>
#include <cstdint>
#include <limits>

typedef uint64_t NanoId;

class NanoIdGenerator {
public:
    NanoIdGenerator() : rng_(rd_()) {}

    NanoId generate() {
        std::uniform_int_distribution<uint64_t> distribution(
            1,
            (std::numeric_limits<uint64_t>::max)()
        );
        
        return distribution(rng_);
    }

    NanoIdGenerator(const NanoIdGenerator&) = delete;
    NanoIdGenerator& operator=(const NanoIdGenerator&) = delete;

private:
    std::random_device rd_;
    std::mt19937_64 rng_;
};
