#ifndef COMPUTECALLBACKGENERATOR
#define COMPUTECALLBACKGENERATOR

#include <RingBuffers/FunctionWrapper.h>
#include <boost/container_hash/hash.hpp>
#include <boost/container_hash/hash_fwd.hpp>
#include <limits>
#include <random>
#include <ranges>
#include <utility>

using URBG = std::mt19937_64;

template<typename value_type, std::uniform_random_bit_generator URNG>
    requires(std::integral<value_type> or std::floating_point<value_type>)
constexpr auto uniform_dist(URNG *rng, value_type min = std::numeric_limits<value_type>::min(),
                            value_type max = std::numeric_limits<value_type>::max()) {
    using Dist = std::conditional_t<std::integral<value_type>, std::uniform_int_distribution<value_type>,
                                    std::uniform_real_distribution<value_type>>;
    return [rng, dist = Dist{min, max}] mutable { return dist(*rng); };
}

template<typename value_type, size_t size>
void fill_array(auto &rng, value_type (&array)[size]) {
    for (auto rn = uniform_dist<value_type>(&rng); auto &&e : array) e = rn();
}

inline size_t compute_1(size_t num) {
    boost::hash_combine(num, 2323442);
    boost::hash_combine(num, 1211113);
    boost::hash_combine(num, 34234235ul);
    return num;
}

inline size_t compute_2(size_t num) {
    boost::hash_combine(num, 24234235ul);
    boost::hash_combine(num, num);
    boost::hash_combine(num, num);
    return num;
}

inline size_t compute_3(size_t num) { return compute_1(compute_2(num)); }

template<size_t fields>
class ComputeFunctor {
public:
    explicit ComputeFunctor(URBG &rng) { fill_array(rng, data); }

    size_t operator()(size_t num) const {
        boost::hash_range(num, std::begin(data), std::end(data));
        return num;
    }

private:
    size_t data[fields];
};

template<size_t fields>
class ComputeFunctor2 {
public:
    explicit ComputeFunctor2(URBG &rng) {
        fill_array(rng, data);
        fill_array(rng, data2);
    }

    size_t operator()(size_t num) const {
        boost::hash_range(num, std::begin(data), std::end(data));
        boost::hash_range(num, std::begin(data2), std::end(data2));
        return num;
    }

private:
    size_t data[fields];
    uint16_t data2[fields];
};

class CallbackGenerator {
public:
    explicit CallbackGenerator(size_t seed) : rng{seed} {}

    void setSeed(size_t seed) { rng.seed(seed); }

    decltype(auto) addCallback(auto &&push_back) {
        auto distUint8 = uniform_dist<uint8_t>(&rng);
        auto distUint16 = uniform_dist<uint16_t>(&rng);
        auto distUint32 = uniform_dist<uint32_t>(&rng);
        auto distUint64 = uniform_dist<uint64_t>(&rng);
        switch (std::invoke(uniform_dist<uint8_t>(&rng, 0, 12))) {
            case 0:
                return push_back([a = distUint64(), b = distUint64(), c = distUint64()](size_t num) {
                    boost::hash_combine(num, num);
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, b);
                    boost::hash_combine(num, c);
                    boost::hash_combine(num, num);
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, b);
                    boost::hash_combine(num, c);
                    boost::hash_combine(num, num);
                    return num;
                });
            case 1:
                return push_back([a = distUint32(), b = distUint32(), c = distUint64(), d = distUint64(),
                                  e = distUint64(), f = distUint64(), g = distUint64()](size_t num) {
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, b);
                    boost::hash_combine(num, c);
                    boost::hash_combine(num, d);
                    boost::hash_combine(num, e);
                    boost::hash_combine(num, f);
                    boost::hash_combine(num, g);
                    return num;
                });
            case 2:
                return push_back(rb::function<compute_1>);
            case 3:
                return push_back(rb::function<compute_2>);
            case 4:
                return push_back(rb::function<compute_3>);
            case 5:
                return push_back(ComputeFunctor<10>{rng});
            case 6:
                return push_back(ComputeFunctor2<10>{rng});
            case 7:
                return push_back(ComputeFunctor<7>{rng});
            case 8:
                return push_back(ComputeFunctor2<5>{rng});
            case 9:
                return push_back(ComputeFunctor<2>{rng});
            case 10:
                return push_back(ComputeFunctor<3>{rng});
            case 11:
                return push_back([a = distUint16()](size_t num) {
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, num);
                    boost::hash_combine(num, a);
                    boost::hash_combine(num, num);
                    return num;
                });
            case 12:
                return push_back([a = distUint8()](size_t num) {
                    boost::hash_combine(num, a);
                    return num;
                });
            default:
                std::unreachable();
        }
    }

private:
    URBG rng;
};

#endif
