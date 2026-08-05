// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

package qaiforge

/*
#include <stdlib.h>
*/
import "C"
import (
	"encoding/json"
	"runtime/cgo"
	"unsafe"
)

// streamCallbackBridge is exported to C so it can be used as a C function pointer.
// It is called by the C++ streaming loop for each token.
// user_data is a *cgo.Handle pointing to a chan StreamChunk.
//
//export streamCallbackBridge
func streamCallbackBridge(chunkJSON *C.char, userData unsafe.Pointer) {
	if userData == nil || chunkJSON == nil {
		return
	}

	// Recover the Go channel from the cgo.Handle
	handle := *(*cgo.Handle)(userData)
	ch, ok := handle.Value().(chan StreamChunk)
	if !ok {
		return
	}

	var chunk StreamChunk
	if err := json.Unmarshal([]byte(C.GoString(chunkJSON)), &chunk); err != nil {
		return
	}

	// Non-blocking send — if the channel buffer is full, drop the chunk
	// (the buffer is 64 deep, so this should not happen in practice)
	select {
	case ch <- chunk:
	default:
	}
}
