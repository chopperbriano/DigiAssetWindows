/*************************************************************************
 * sharedconstants.h
 *
 * Compile-time constants shared across the libjson-rpc-cpp transport
 * layer (part of the bundled JSON-RPC library used by the node's and
 * pool server's RPC interfaces). Defines the line delimiter and socket
 * read-buffer size used by the framed stream/socket connectors, plus
 * pulls in the platform header that provides PATH_MAX where available.
 ************************************************************************/

#ifndef SHAREDCONSTANTS_H
#define SHAREDCONSTANTS_H

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

#ifdef __linux__
#include <linux/limits.h>
#endif

// Byte (newline, 0x0A) that marks the end of one framed JSON-RPC message on a stream/socket.
#define DEFAULT_DELIMITER_CHAR char(0x0A)
// Size in bytes of the temporary read buffer used when draining an incoming connection.
#define DEFAULT_BUFFER_SIZE 1024

#endif // SHAREDCONSTANTS_H
