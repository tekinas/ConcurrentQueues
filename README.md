# Concurrent Queues
## FunctionQueueSCSP
A single producer, single consumer concurrent queue which stores callable objects of arbitrary type and size.
## FunctionQueueMCSP
A single producer, multiple consumer concurrent queue which stores callable objects of arbitrary type and size.
## FunctionQueue
An unsynchronized queue which stores callable objects of arbitrary type and size. It should be accessed mutually exclusively by the reader or writer threads.
## ObjectQueueSCSP
A single producer, single consumer concurrent queue which stores objects of a fixed type.
## ObjectQueueMCSP
A single producer, multiple consumer concurrent queue which stores objects of a fixed type.
## BufferQueueSCSP
A single producer, single consumer concurrent queue which stores buffers of arbitrary size and alignment.
## BufferQueueMCSP
A single producer, multiple consumer concurrent queue which stores buffers of arbitrary size and alignment.
## FunctionWrapper
Convert a function pointer known at compile time to a callable type without any state.  It's a constexpr variable template which takes as its template parameter a function pointer and and invoke it by perfect forwarding its arguments to the function pointer. This is intended to be used with Function queues to save space when storing function pointers known at compile time.
