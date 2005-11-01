/* Legal Notice: The source code contained in this file has been derived from
   the source code of Encryption for the Masses 2.02a, which is Copyright (c)
   1998-99 Paul Le Roux and which is covered by the 'License Agreement for
   Encryption for the Masses'. Modifications and additions to that source code
   contained in this file are Copyright (c) 2004-2005 TrueCrypt Foundation and
   Copyright (c) 2004 TrueCrypt Team, and are covered by TrueCrypt License 2.0
   the full text of which is contained in the file License.txt included in
   TrueCrypt binary and source code distribution archives.  */

#include "Tcdefs.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include "Random.h"
#endif

#include "Crypto.h"
#include "Endian.h"
#include "Volumes.h"

#include "Pkcs5.h"
#include "Crc.h"


#define NBR_KEY_BYTES_TO_DISPLAY	16


// Volume header structure:
//
// Offset	Length	Description
// ------------------------------------------
// Unencrypted:
// 0		64		Salt
// Encrypted:
// 64		4		ASCII string 'TRUE'
// 68		2		Header version
// 70		2		Required program version
// 72		4		CRC32 of disk IV and key
// 76		8		Volume creation time
// 84		8		Header creation time
// 92		8		Size of hidden volume in bytes (0 = normal volume)
// 100		156		Unused
// 256		32		Disk IV
// 288		224		Disk key

int
VolumeReadHeader (char *encryptedHeader, Password *password, PCRYPTO_INFO *retInfo)
{
	char header[HEADER_SIZE];
	unsigned char *input = (unsigned char *) header;
	KEY_INFO keyInfo;
	PCRYPTO_INFO cryptoInfo;
	int nKeyLen;
	char dk[DISKKEY_SIZE];
	int pkcs5;
	int headerVersion, requiredVersion;
	int status;

	cryptoInfo = *retInfo = crypto_open ();
	if (cryptoInfo == NULL)
		return ERR_OUTOFMEMORY;

	crypto_loadkey (&keyInfo, password->Text, password->Length);

	// PKCS5 is used to derive header key and IV from user password
	memcpy (keyInfo.key_salt, encryptedHeader + HEADER_USERKEY_SALT, PKCS5_SALT_SIZE);

	// Test all available PKCS5 PRFs
	for (pkcs5 = 1; pkcs5 <= LAST_PRF_ID; pkcs5++)
	{
		keyInfo.noIterations = get_pkcs5_iteration_count (pkcs5);

		switch (pkcs5)
		{
		case SHA1:
			derive_key_sha1 (keyInfo.userKey, keyInfo.keyLength, keyInfo.key_salt,
				PKCS5_SALT_SIZE, keyInfo.noIterations, dk, DISK_IV_SIZE + EAGetLargestKey());
			break;

		case RIPEMD160:
			derive_key_ripemd160 (keyInfo.userKey, keyInfo.keyLength, keyInfo.key_salt,
				PKCS5_SALT_SIZE, keyInfo.noIterations, dk, DISK_IV_SIZE + EAGetLargestKey());
			break;

		case WHIRLPOOL:
			derive_key_whirlpool (keyInfo.userKey, keyInfo.keyLength, keyInfo.key_salt,
				PKCS5_SALT_SIZE, keyInfo.noIterations, dk, DISK_IV_SIZE + EAGetLargestKey());
			break;
		} 

		// Test all available encryption algorithms
		for (cryptoInfo->ea = EAGetFirst (); cryptoInfo->ea != 0; cryptoInfo->ea = EAGetNext (cryptoInfo->ea))
		{
			// Copy header for decryption and init an encryption algorithm
			memcpy (header, encryptedHeader, SECTOR_SIZE);  
			memcpy (cryptoInfo->iv, dk, DISK_IV_SIZE);

			status = EAInit (cryptoInfo->ea, dk + DISK_IV_SIZE, cryptoInfo->ks);
			if (status == ERR_CIPHER_INIT_FAILURE)
				goto err;

			input = header;

			// Try to decrypt header 

			DecryptBuffer ((unsigned __int32 *) (header + HEADER_ENCRYPTEDDATA), HEADER_ENCRYPTEDDATASIZE,
				cryptoInfo->ks, cryptoInfo->iv, &cryptoInfo->iv[8], cryptoInfo->ea);

			input += HEADER_ENCRYPTEDDATA;

			// Magic 'TRUE'
			if (mgetLong (input) != 0x54525545)
				continue;

			// Header version
			headerVersion = mgetWord (input);

			// Required program version
			requiredVersion = mgetWord (input);

			// Check CRC of disk IV and key
			if (mgetLong (input) != crc32 (header + HEADER_DISKKEY, DISKKEY_SIZE))
				continue;

			// Now we have the correct password, cipher, hash algorithm, and volume type

			// Check the version required to handle this volume
			if (requiredVersion > VERSION_NUM)
			{
				status = ERR_NEW_VERSION_REQUIRED;
				goto err;
			}

			// Volume creation time
			cryptoInfo->volume_creation_time = mgetInt64 (input);

			// Header creation time
			cryptoInfo->header_creation_time = mgetInt64 (input);

			// Hidden volume size (if any)
			cryptoInfo->hiddenVolumeSize = mgetInt64 (input);

			// Disk key
			nKeyLen = DISKKEY_SIZE;
			memcpy (keyInfo.key, header + HEADER_DISKKEY, nKeyLen);

			memcpy (cryptoInfo->master_key, keyInfo.key, nKeyLen);
			memcpy (cryptoInfo->key_salt, keyInfo.key_salt, PKCS5_SALT_SIZE);
			cryptoInfo->pkcs5 = pkcs5;
			cryptoInfo->noIterations = keyInfo.noIterations;

			// Init the encryption algorithm with the decrypted master key
			status = EAInit (cryptoInfo->ea, keyInfo.key + DISK_IV_SIZE, cryptoInfo->ks);
			if (status == ERR_CIPHER_INIT_FAILURE)
				goto err;

			// Data area IV seed
			memcpy (cryptoInfo->iv, keyInfo.key, DISK_IV_SIZE);

			// Clear out the temp. key buffers
			burn (dk, sizeof(dk));

			return 0;

		}
	}
	status = ERR_PASSWORD_WRONG;

err:
	crypto_close(cryptoInfo);
	burn (&keyInfo, sizeof (keyInfo));
	return status;
}

#ifndef DEVICE_DRIVER

#ifdef VOLFORMAT
extern BOOL showKeys;
extern HWND hDiskKey;
extern HWND hHeaderKey;
#endif

#ifdef _WIN32

// VolumeWriteHeader:
// Creates volume header in memory
int
VolumeWriteHeader (char *header, int ea, Password *password,
		   int pkcs5, char *masterKey, unsigned __int64 volumeCreationTime, PCRYPTO_INFO * retInfo,
		   unsigned __int64 hiddenVolumeSize, BOOL bWipeMode)
{
	unsigned char *p = (unsigned char *) header;
	static KEY_INFO keyInfo;

	int nUserKeyLen = password->Length;
	PCRYPTO_INFO cryptoInfo = crypto_open ();
	static char dk[DISKKEY_SIZE];
	int x;
	int retVal = 0;

	if (cryptoInfo == NULL)
		return ERR_OUTOFMEMORY;

	memset (header, 0, SECTOR_SIZE);
	VirtualLock (&keyInfo, sizeof (keyInfo));
	VirtualLock (&dk, sizeof (dk));

	/* Encryption setup */

	// If necessary, generate the master key, IV and whitening seeds
	if(masterKey == 0)
		RandgetBytes (keyInfo.key, DISKKEY_SIZE, TRUE);
	else
		memcpy (keyInfo.key, masterKey, DISKKEY_SIZE);

	// User key 
	memcpy (keyInfo.userKey, password->Text, nUserKeyLen);
	keyInfo.keyLength = nUserKeyLen;
	keyInfo.noIterations = get_pkcs5_iteration_count (pkcs5);

	// User selected encryption algorithm
	cryptoInfo->ea = ea;

	// Salt for header key derivation 
	RandgetBytes (keyInfo.key_salt, PKCS5_SALT_SIZE, !bWipeMode);

	// PKCS5 is used to derive the header key and IV from the password
	switch (pkcs5)
	{
	case SHA1:
		derive_key_sha1 (keyInfo.userKey, keyInfo.keyLength, keyInfo.key_salt,
			PKCS5_SALT_SIZE, keyInfo.noIterations, dk, DISK_IV_SIZE + EAGetLargestKey());
		break;

	case RIPEMD160:
		derive_key_ripemd160 (keyInfo.userKey, keyInfo.keyLength, keyInfo.key_salt,
			PKCS5_SALT_SIZE, keyInfo.noIterations, dk, DISK_IV_SIZE + EAGetLargestKey());
		break;

	case WHIRLPOOL:
		derive_key_whirlpool (keyInfo.userKey, keyInfo.keyLength, keyInfo.key_salt,
			PKCS5_SALT_SIZE, keyInfo.noIterations, dk, DISK_IV_SIZE + EAGetLargestKey());
		break;
	} 

	/* Header setup */

	// Salt
	mputBytes (p, keyInfo.key_salt, PKCS5_SALT_SIZE);	

	// Magic
	mputLong (p, 'TRUE');

	// Header version
	mputWord (p, VOLUME_HEADER_VERSION);

	// Required program version to handle this volume
	mputWord (p, VOL_REQ_PROG_VERSION);

	// CRC of disk key
	x = crc32(keyInfo.key, DISKKEY_SIZE);
	mputLong (p, x);

	// Time
	{
		SYSTEMTIME st;
		FILETIME ft;

		// Volume creation time
		if (volumeCreationTime == 0)
		{
			GetLocalTime (&st);
			SystemTimeToFileTime (&st, &ft);
		}
		else
		{
			ft.dwHighDateTime = (DWORD)(volumeCreationTime >> 32);
			ft.dwLowDateTime = (DWORD)volumeCreationTime;
		}
		mputLong (p, ft.dwHighDateTime);
		mputLong (p, ft.dwLowDateTime);

		// Header modification time/date
		GetLocalTime (&st);
		SystemTimeToFileTime (&st, &ft);
		mputLong (p, ft.dwHighDateTime);
		mputLong (p, ft.dwLowDateTime);
	}

	// Hidden volume size
	cryptoInfo->hiddenVolumeSize = hiddenVolumeSize;
	mputInt64 (p, cryptoInfo->hiddenVolumeSize);

	// Disk key and IV
	memcpy (header + HEADER_DISKKEY, keyInfo.key, DISKKEY_SIZE);


	/* Header encryption */

	memcpy (cryptoInfo->iv, dk, DISK_IV_SIZE);
	retVal = EAInit (cryptoInfo->ea, dk + DISK_IV_SIZE, cryptoInfo->ks);
	if (retVal != 0)
		return retVal;

	EncryptBuffer ((unsigned __int32 *) (header + HEADER_ENCRYPTEDDATA), HEADER_ENCRYPTEDDATASIZE,
			cryptoInfo->ks, cryptoInfo->iv, &cryptoInfo->iv[8], cryptoInfo->ea);


	/* cryptoInfo setup for further use (disk format) */

	// Init with the master key 
	retVal = EAInit (cryptoInfo->ea, keyInfo.key + DISK_IV_SIZE, cryptoInfo->ks);
	if (retVal != 0)
		return retVal;

	// Disk IV
	memcpy (cryptoInfo->iv, keyInfo.key, DISK_IV_SIZE);


#ifdef VOLFORMAT
	if (showKeys)
	{
		char tmp[64];
		BOOL dots3 = FALSE;
		int i, j;

		j = EAGetKeySize (ea);

		if (j > NBR_KEY_BYTES_TO_DISPLAY)
		{
			dots3 = TRUE;
			j = NBR_KEY_BYTES_TO_DISPLAY;
		}

		tmp[0] = 0;
		for (i = 0; i < j; i++)
		{
			char tmp2[8] =
			{0};
			sprintf (tmp2, "%02X", (int) (unsigned char) keyInfo.key[i + DISK_IV_SIZE]);
			strcat (tmp, tmp2);
		}

		if (dots3)
		{
			strcat (tmp, "...");
		}


		SetWindowText (hDiskKey, tmp);

		tmp[0] = 0;
		for (i = 0; i < NBR_KEY_BYTES_TO_DISPLAY; i++)
		{
			char tmp2[8];
			sprintf (tmp2, "%02X", (int) (unsigned char) dk[DISK_IV_SIZE + i]);
			strcat (tmp, tmp2);
		}

		if (dots3)
		{
			strcat (tmp, "...");
		}

		SetWindowText (hHeaderKey, tmp);
	}
#endif

	burn (dk, sizeof(dk));
	burn (&keyInfo, sizeof (keyInfo));

	*retInfo = cryptoInfo;
	return 0;
}

#endif				/* WIN32 */

#endif				/* !NT4_DRIVER */
