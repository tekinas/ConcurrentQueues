#ifndef MOVE_FORWARD_H
#define MOVE_FORWARD_H

#define mov(...) static_cast<std::remove_reference_t<decltype(__VA_ARGS__)> &&>(__VA_ARGS__)

#define fwd(...) static_cast<decltype(__VA_ARGS__) &&>(__VA_ARGS__)

#endif

