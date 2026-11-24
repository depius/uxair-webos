package main

/*
#include <stdint.h>
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"

	phase1 "github.com/jkiddo/go-atv/uxairphase1"
)

func transport(handle C.uintptr_t) *phase1.Transport {
	return cgo.Handle(handle).Value().(*phase1.Transport)
}

// Each export contains panics at the ABI boundary. C closes the socket and
// discards its handle on any failure. The only persisted value is an integer.
func contain(result *C.int) {
	if recover() != nil {
		*result = -1
	}
}

//export uxair_vc_version
func uxair_vc_version() C.int { return 1 }

//export uxair_vc_new
func uxair_vc_new() (handle C.uintptr_t) {
	defer func() {
		if recover() != nil {
			handle = 0
		}
	}()
	state, err := phase1.NewTransport()
	if err != nil {
		return 0
	}
	return C.uintptr_t(cgo.NewHandle(state))
}

//export uxair_vc_free
func uxair_vc_free(handle C.uintptr_t) (result C.int) {
	defer contain(&result)
	transport(handle).Close()
	cgo.Handle(handle).Delete()
	return 0
}

//export uxair_vc_sap
func uxair_vc_sap(handle C.uintptr_t, input *C.uchar, n C.int, output *C.uchar) (result C.int) {
	defer contain(&result)
	if input == nil || output == nil || (n != 16 && n != 164) {
		return -1
	}
	reply, err := transport(handle).Exchange(unsafe.Slice((*byte)(input), int(n)))
	if err != nil || len(reply) > 142 {
		return -1
	}
	defer clear(reply)
	return C.int(copy(unsafe.Slice((*byte)(output), 142), reply))
}

//export uxair_vc_upgrade
func uxair_vc_upgrade(handle C.uintptr_t, input *C.uchar, n C.int, secret *C.uchar) (result C.int) {
	defer contain(&result)
	if input == nil || n != 120 || secret == nil {
		return -2
	}
	err := transport(handle).Upgrade(unsafe.Slice((*byte)(input), 120), unsafe.Slice((*byte)(secret), 32))
	if err == phase1.ErrEnvelope {
		return -2
	}
	if err != nil {
		return -1
	}
	return 0
}

//export uxair_vc_open
func uxair_vc_open(handle C.uintptr_t, input *C.uchar, n C.int, output *C.uchar) (result C.int) {
	defer contain(&result)
	if input == nil || output == nil || n < phase1.RecordOverhead || n > phase1.MaxRecordPayload+phase1.RecordOverhead {
		return -1
	}
	reply, err := transport(handle).Open(unsafe.Slice((*byte)(input), int(n)))
	if err != nil || len(reply) != int(n)-phase1.RecordOverhead {
		return -1
	}
	defer clear(reply)
	return C.int(copy(unsafe.Slice((*byte)(output), len(reply)), reply))
}

//export uxair_vc_seal
func uxair_vc_seal(handle C.uintptr_t, input *C.uchar, n C.int, output *C.uchar) (result C.int) {
	defer contain(&result)
	if input == nil || output == nil || n < 0 || n > phase1.MaxRecordPayload {
		return -1
	}
	reply, err := transport(handle).Seal(unsafe.Slice((*byte)(input), int(n)))
	if err != nil || len(reply) != int(n)+phase1.RecordOverhead {
		return -1
	}
	return C.int(copy(unsafe.Slice((*byte)(output), len(reply)), reply))
}

func main() {}
