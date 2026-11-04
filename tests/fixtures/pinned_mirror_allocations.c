/*
 * Copyright (c) 2019 dsafa22, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *==================================================================
 * modified by fduncanh 2021-2023
 */
/* Allocation excerpts from UxPlay 9f3c2bbc; executable harness in network_bounds_test.py. */
/* SECTION header */
            int payload_size = byteutils_get_int(packet, 0);
/* SECTION raw */
                payload = malloc(payload_size);
/* SECTION prepend */
                    payload_out = (unsigned char*) malloc(payload_size + sps_pps_len);
                    if (!payload_out) {
                        printf("Memory allocation failed (payload_out)\n");
                        exit(1);
                    }
/* SECTION plain */
                    payload_out = (unsigned char*)  malloc(payload_size);
/* SECTION parameter_a */
                    sps_pps = (unsigned char*) malloc(sps_pps_len);
/* SECTION parameter_b */
                    sps_pps = (unsigned char*) malloc(sps_pps_len);
/* SECTION terminate */
            if (unsupported_codec) {
                break;
            }
/* SECTION cleanup */
    /* Close the stream file descriptor */
/* SECTION end */
