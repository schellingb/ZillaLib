/*
  ZillaLib
  Copyright (C) 2010-2020 Bernhard Schelling

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifndef __ZL_DATA__
#define __ZL_DATA__

#if defined(_MSC_VER) && (_MSC_VER <= 1200)
#pragma warning(disable:4786) //'Some STL template class' : identifier was truncated to '255' characters in the debug information
#pragma warning(disable:4530) //C++ exception handler used, but unwind semantics are not enabled
#endif

#include <vector>
#include <map>
#include "ZL_Math.h"
#include "ZL_File.h"
#include "ZL_Application.h"

#if (defined(__cplusplus) && (__cplusplus > 199711 || _MSC_VER >= 1800) && (!defined(__GNUC__) || defined(__clang__) || __GNUC__ > 4))
#define ZL_HAVE_TYPE_TRAITS
#include <type_traits>
#endif

//JSON format parser and writer
struct ZL_Json
{
	struct Iterator
	{
		operator bool () const { return (cursor >= begin && cursor < end); }
		ZL_Json& operator*() { return *(ZL_Json*)(void*)(cursor); }
		ZL_Json* operator->() { return (ZL_Json*)(void*)(cursor); }
		Iterator & operator++() { ++cursor; return *this; }
		Iterator & operator--() { --cursor; return *this; }
		size_t GetIndex() { return (cursor - begin); }
		private: struct ZL_JSON_Impl **cursor, **begin, **end; friend struct ZL_Json;
	};

	ZL_Json();
	ZL_Json(const char *json);
	ZL_Json(const char *json, size_t len);
	ZL_Json(const ZL_String &json);
	ZL_Json(const ZL_File &file);
	~ZL_Json();
	ZL_Json(const ZL_Json &source);
	ZL_Json &operator =(const ZL_Json &source) { SetReference(source); return *this; }
	ZL_Json &operator =(const char* NewString) { SetString(NewString); return *this; }
	ZL_Json &operator =(scalar NewFloat) { SetFloat(NewFloat); return *this; }
	ZL_Json &operator =(int NewInt) { SetInt(NewInt); return *this; }
	ZL_Json &operator =(bool NewBool) { SetBool(NewBool); return *this; }

	operator bool()  const { return (impl!=NULL); }
	bool operator!() const { return (impl==NULL); }
	bool operator==(const ZL_Json &b) const { return (impl==b.impl); }
	bool operator!=(const ZL_Json &b) const { return (impl!=b.impl); }

	enum eType { TYPE_NULL, TYPE_OBJECT, TYPE_ARRAY, TYPE_STRING, TYPE_NUMBER, TYPE_TRUE, TYPE_FALSE };
	eType GetType() const;

	//for arrays and objects
	size_t Size() const; //number of array/object children
	Iterator GetIterator() const;
	std::vector<ZL_Json> GetChildren() const;
	ZL_Json GetChild(size_t index) const;
	ZL_Json GetChildOf(const char* child_key, size_t index) const; //returns child of child
	ZL_Json GetByKey(const char* key) const; //returns uninitialized element if not exist (checkable with ! operator)

	//get the value of this element
	const char* GetString(const char* default_value = NULL) const; //return default_value if not TYPE_STRING
	scalar GetFloat(scalar default_value = 0) const; //return default_value if not TYPE_NUMBER
	int GetInt(int default_value = 0) const; //return default_value if not TYPE_NUMBER
	bool GetBool() const;
	bool IsNull() const;
	bool HasKey(const char* key) const;
	const char* GetKey() const; //if value in object

	//get a value of a child element
	const char* GetStringOf(const char* child_key, const char* default_value = NULL) const;
	const char* GetStringOf(size_t child_index, const char* default_value = NULL) const;
	scalar GetFloatOf(const char* child_key, scalar default_value = 0) const;
	scalar GetFloatOf(size_t child_index, scalar default_value = 0) const;
	int GetIntOf(const char* child_key, int default_value = 0) const;
	int GetIntOf(size_t child_index, int default_value = 0) const;
	bool GetBoolOf(const char* child_key) const;
	bool GetBoolOf(size_t child_index) const;

	ZL_Json operator()(size_t index) const { return GetChild(index); } //get nth child
	ZL_Json operator[](const char* key); //get/create object key-value
	ZL_Json Add(); //add element to array

	void SetString(const char* NewString);
	void SetFloat(scalar NewFloat);
	void SetInt(int NewInt);
	void SetBool(bool NewBool);
	void SetNull();
	void SetReference(const ZL_Json &source);
	void SetKey(const char* NewKey);

	Iterator Erase(Iterator it);
	bool Erase(const char* key); //erases only the first matching object key
	bool Erase(const ZL_Json &child);
	bool EraseAt(size_t index);
	void Clear(); //only clears array/object, use SetNull otherwise

	//return json formatted string
	ZL_String ToString(bool PrettyPrint = true) const;
	operator ZL_String () const { return ToString(); }

	//for C++11 range-based for loops
	Iterator begin() const { return GetIterator(); }
	Iterator end() const { Iterator it = GetIterator(); it.cursor = it.end; return it; }

	private: struct ZL_JSON_Impl* impl; ZL_Json(struct ZL_JSON_Impl*);
};

//XML format parser and writer
struct ZL_Xml
{
	ZL_Xml();
	ZL_Xml(const ZL_File &file);
	ZL_Xml(const ZL_String &xml);
	~ZL_Xml();
	ZL_Xml(const ZL_Xml &source);
	ZL_Xml &operator =(const ZL_Xml &source);
	bool operator!() const { return (impl==NULL); }
	bool operator==(const ZL_Xml &b) const { return (impl==b.impl); }
	bool operator!=(const ZL_Xml &b) const { return (impl!=b.impl); }

	ZL_Xml AddChild(ZL_Xml &Child);
	void AddChild(ZL_Xml *pChild);
	ZL_Xml AddChild(const ZL_String& name);
	void RemoveChild(const ZL_Xml *pChild);
	void RemoveChildren(const ZL_String& name);

	const ZL_String& GetName() const;
	void SetName(const ZL_String& newname);

	const ZL_String& GetText() const;
	void SetText(const ZL_String& newtext);

	ZL_String ToString() const;
	operator ZL_String () const { return ToString(); }

	void SetParameter(const ZL_String &name, const ZL_String &paramval = ZL_String::EmptyString);
	void RemoveParameter(const ZL_String &name);
	bool HasParameter(const ZL_String &name) const;
	std::map<ZL_String, ZL_String>& GetParameters();
	std::vector<ZL_Xml> GetChildren() const;
	std::vector<ZL_Xml> GetChildrenByName(const ZL_String &name) const;

	ZL_String& operator[](const ZL_String &paramname) const;
	std::allocator<ZL_String>::reference operator[](const ZL_String &paramname);
	ZL_Xml operator()(const ZL_String &childname);

	private: struct ZL_Xml_Impl* impl; ZL_Xml(struct ZL_Xml_Impl*);
};

//Manages Base64 encoding and decoding
struct ZL_Base64
{
	//Encode a buffer to a string base64 encoded
	static ZL_String Encode(const void* InputBuffer, size_t BufSize);

	//Decode a string base64 encoded into a vector and return length of decoded data
	static size_t Decode(const ZL_String& Base64Data, std::vector<unsigned char>& output);

	//Decode a string base64 encoded into a given buffer, returned length of decoded data will be of max BufSize
	static size_t Decode(const ZL_String& Base64Data, void* OutputBuffer, size_t BufSize);

	//Compute maximum possible size of decoded base64 stream (actual decoded might be 1 or 2 bytes smaller)
	static size_t DecodedMaxSize(size_t EncodedSize);

	//Test if a string is well Base64 encoded
	static bool IsBase64(const ZL_String& Base64Data);
};

//Data compression
struct ZL_Compression
{
	enum { NO_COMPRESSION = 0, BEST_SPEED = 1, BEST_COMPRESSION = 9, DEFAULT_COMPRESSION = 6 };

	//Compress data and append it to an automatically sized output vector and return the compressed size (if ReserveMaxCompressSize is false, multiple re-allocations might be made)
	static size_t Compress(const void* DecompressedBuffer, size_t DecompressedSize, std::vector<unsigned char>& Output, int Level = DEFAULT_COMPRESSION, bool ReserveMaxCompressSize = false);
	static inline size_t Compress(const std::vector<unsigned char>& Decompressed, std::vector<unsigned char>& Output, int Level = DEFAULT_COMPRESSION, bool ReserveMaxCompressSize = false) { return Compress((Decompressed.empty() ? NULL : &Decompressed[0]), Decompressed.size(), Output, Level, ReserveMaxCompressSize); }

	//Returns the upper bound on the amount of data that could be generated by calling Compress
	static size_t CompressMaxSize(size_t DecompressedSize);

	//Compress data into a fixed size buffer and returns true if the given buffer was large enough (CompressedSize will be modified to actual size)
	static bool Compress(const void* DecompressedBuffer, size_t DecompressedSize, const void* CompressedBuffer, size_t* CompressedSize, int Level = DEFAULT_COMPRESSION);

	//Decompress and append data to an automatically sized output vector and return size of output (if DecompressedSizeHint is not large enough, multiple re-allocations might be made)
	static size_t Decompress(const void* CompressedBuffer, size_t CompressedSize, std::vector<unsigned char>& Output, size_t DecompressedSizeHint = 0);
	static inline size_t Decompress(const std::vector<unsigned char>& Compressed, std::vector<unsigned char>& Output, size_t DecompressedSizeHint = 0) { return Decompress((Compressed.empty() ? NULL : &Compressed[0]), Compressed.size(), Output, DecompressedSizeHint); }

	//Decompress data into a fixed size buffer and returns true if the given buffer was large enough (DecompressedSize will be modified to actual size)
	static bool Decompress(const void* CompressedBuffer, size_t CompressedSize, const void* DecompressedBuffer, size_t* DecompressedSize);
};

//Calculate checksums
struct ZL_Checksum
{
	static unsigned int CRC32(const void* Data, size_t DataSize, unsigned int extend = 0);
	static unsigned int Fast(const void* Data, size_t DataSize);
	static unsigned int Fast4(const void* Data, size_t DataSize); //DataSize must be 4 byte aligned
	static void SHA1(const void* Data, size_t DataSize, unsigned char OutResult[20]);
};

inline size_t ZL_Hash(i64 x) { x *= 0xff51afd7ed558ccd; return (size_t)(x ^ (x >> 32)); }
inline size_t ZL_Hash(u64 x) { x *= 0xff51afd7ed558ccd; return (size_t)(x ^ (x >> 32)); }
inline size_t ZL_Hash(int x) { x *= 0x85ebca6b; return (size_t)(x ^ (x >> 12)); }
inline size_t ZL_Hash(unsigned int x) { x *= 0x85ebca6b; return (size_t)(x ^ (x >> 12)); }
#ifdef ZL_IS_64_BIT
inline size_t ZL_Hash(const void* x) { return ZL_Hash((u64)x); ZL_STATIC_ASSERT(sizeof(x) == 8); }
#else
inline size_t ZL_Hash(const void* x) { return ZL_Hash((unsigned int)x); ZL_STATIC_ASSERT(sizeof(x) == 4); }
#endif

//A name that is a checksum of a string for easy lookups and comparison (does not keep the string itself, just the 4 byte checksum)
struct ZL_NameID
{
	ZL_NameID() : IDValue(0) {}
	ZL_NameID(const char* str) : IDValue(ZL_Checksum::CRC32(str, strlen(str))) {}
	ZL_NameID(const char* str, size_t len) : IDValue(ZL_Checksum::CRC32(str, len)) {}
	ZL_NameID(const ZL_String& str) : IDValue(ZL_Checksum::CRC32(str.c_str(), str.length())) {}
	bool operator!() const { return (IDValue == 0); }
	bool operator==(const ZL_NameID &b) const { return (IDValue==b.IDValue); }
	bool operator!=(const ZL_NameID &b) const { return (IDValue!=b.IDValue); }
	bool operator<(const ZL_NameID& b) const { return (IDValue<b.IDValue); }
	unsigned int IDValue;
};

//A hash map, TKey needs a ZL_Hash function, a == operator, and a ! operator that returns true when memory is zeroed, both TKey and TVal need to be trivially copyable
template <typename TKey, typename TVal> struct ZL_TMap
{
	TVal NotFoundValue;

	ZL_TMap() : NotFoundValue(TVal()), len(0), cap(0), keys(NULL), vals(NULL)
	{
		#ifdef ZL_HAVE_TYPE_TRAITS
		ZL_STATIC_ASSERTMSG(std::is_trivially_copyable<TKey>::value, Map_key_type_needs_to_be_trivially_copyable);
		ZL_STATIC_ASSERTMSG(std::is_trivially_copyable<TVal>::value, Map_value_type_needs_to_be_trivially_copyable);
		#endif
	}
	~ZL_TMap() { free(keys); free(vals); }

	TVal& Get(TKey key)
	{
		ZL_ASSERT(!!key);
		if (len == 0) return NotFoundValue;
		for (size_t i = (size_t)ZL_Hash(key);; i++)
		{
			if (keys[i &= cap - 1] == key) return vals[i];
			if (!keys[i]) return NotFoundValue;
		}
	}

	TVal& Put(TKey key, const TVal& val)
	{
		ZL_ASSERT(!!key);
		if (2*len >= cap) Grow(2*cap);
		for (size_t i = (size_t)ZL_Hash(key);; i++)
		{
			if (!keys[i &= cap - 1]) { len++; keys[i] = key; return vals[i] = val; }
			if (keys[i] == key) return vals[i] = val;
		}
	}

	bool Remove(TKey key)
	{
		ZL_ASSERT(!!key);
		if (len == 0) return false;
		for (size_t i = (size_t)ZL_Hash(key);; i++)
		{
			if (keys[i &= cap - 1] == key) { keys[i] = 0; len--; return true; }
			if (!keys[i]) return false;
		}
	}

	void Clear() { memset(keys, len = 0, cap * sizeof(TKey)); }

private:
	size_t len, cap;
	TKey *keys;
	TVal *vals;

	void Grow(size_t new_cap)
	{
		TKey *oldKeys = keys;
		TVal *oldVals = vals;
		size_t oldCap = cap;
		len  = 0;
		cap  = (new_cap < 16 ? 16 : new_cap);
		keys = (TKey *)calloc(cap, sizeof(TKey));
		vals = (TVal *)malloc(cap * sizeof(TVal));
		for (size_t i = 0; i < oldCap; i++) if (!!oldKeys[i]) Put(oldKeys[i], oldVals[i]);
		free(oldKeys);
		free(oldVals);
	}
};

//A hash set, TKey needs to be trivially copyable, a ZL_Hash function and a == operator, and also needs a ! operator that returns true when memory is zeroed
template <typename TKey> struct ZL_TSet
{
	ZL_TSet() : len(0), cap(0), keys(NULL)
	{
		#ifdef ZL_HAVE_TYPE_TRAITS
		ZL_STATIC_ASSERTMSG(std::is_trivial<TKey>::value, Set_key_type_needs_to_be_trivially_copyable);
		#endif
	}
	~ZL_TSet() { free(keys); }

	bool Get(TKey key)
	{
		ZL_ASSERT(!!key);
		if (len == 0) return false;
		for (size_t i = (size_t)ZL_Hash(key);; i++)
		{
			if (keys[i &= cap - 1] == key) return true;
			if (!keys[i]) return false;
		}
	}

	bool Put(TKey key)
	{
		ZL_ASSERT(!!key);
		if (2*len >= cap) Grow(2*cap);
		for (size_t i = (size_t)ZL_Hash(key);; i++)
		{
			if (!keys[i &= cap - 1]) { len++; keys[i] = key; return true; }
			if (keys[i] == key) return false;
		}
	}

	bool Remove(TKey key)
	{
		ZL_ASSERT(!!key);
		if (len == 0) return false;
		for (size_t i = (size_t)ZL_Hash(key);; i++)
		{
			if (keys[i &= cap - 1] == key) { keys[i] = 0; len--; return true; }
			if (!keys[i]) return false;
		}
	}

	void Clear() { memset(keys, len = 0, cap * sizeof(TKey)); }

private:
	size_t len, cap;
	TKey *keys;

	void Grow(size_t new_cap)
	{
		TKey *oldKeys = keys;
		size_t oldCap = cap;
		len  = 0;
		cap  = (new_cap < 16 ? 16 : new_cap);
		keys = (TKey *)calloc(cap, sizeof(TKey));
		for (size_t i = 0; i < oldCap; i++) if (!!oldKeys[i]) Put(oldKeys[i]);
		free(oldKeys);
	}
};

#endif //__ZL_DATA__
