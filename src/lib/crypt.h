/*

Read Route Record

Copyright (C) 2018 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef VL_CRYPT_H
#define VL_CRYPT_H

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "../global.h"

/*
 * These functions should not be used directly.
 * Use src/modules/crypt.*-functions instead for thread-safety.
 */

struct vl_crypt {
	EVP_PKEY *evp_key;
	char key_bin[SHA512_DIGEST_LENGTH];
	char key[SHA512_DIGEST_LENGTH * 2 + 1];
	char iv_bin[SHA256_DIGEST_LENGTH];
	char iv[SHA256_DIGEST_LENGTH * 2 + 1];
	EVP_CIPHER_CTX *ctx;
};

void vl_crypt_initialize_locks(void);
void vl_crypt_free_locks(void);
int vl_crypt_global_lock(void);
void vl_crypt_global_unlock(void *arg); // Need arg because of pthread_cleanup_push
struct vl_crypt *vl_crypt_new(void);
void vl_crypt_free(struct vl_crypt *crypt);
int vl_crypt_load_key(struct vl_crypt *crypt, const char *filename);
int vl_crypt_set_iv_from_hex(struct vl_crypt *crypt, const char *iv_string);
int vl_crypt_aes256 (
		struct vl_crypt *crypt,
		const void *source, unsigned int source_length,
		void **target, unsigned int *target_length
);
int vl_decrypt_aes256 (struct vl_crypt *crypt,
		const void *source, unsigned int source_length,
		void **target, unsigned int *target_length
);

#endif
