/*
  ZillaLib
  Copyright (C) 2010-2026 Bernhard Schelling

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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ZL_Data.h"
#include "ZL_Application.h"
#include "ZL_Impl.h"

struct ZL_JSON_Impl : public ZL_Impl
{
	enum { TYPE_PROXY = 14, PARSE_ERROR = 15, FLAG_KEYNEEDFREE = 1, FLAG_STRINGNEEDFREE = 2, FLAG_ISROOT = 4 };
	union { struct { unsigned char Type, Flags; unsigned short ObjectKeyLen; }; unsigned int TypeAndFlags; };
	char* ObjectKey;
	union { char* DataString; std::vector<ZL_JSON_Impl*>* DataChildren; scalar DataNumber; ZL_JSON_Impl* DataProxy; };

	ZL_JSON_Impl() : TypeAndFlags(0), ObjectKey(NULL), DataChildren(NULL) { ZL_STATIC_ASSERTMSG(ZL_Json::TYPE_NULL==0, null_type_must_be_zero_for_TypeAndFlags_init); }
	ZL_JSON_Impl(const char* src) { Construct(src, strlen(src)); }
	ZL_JSON_Impl(const char *src, size_t len) { Construct(src, len); }
	ZL_JSON_Impl(const ZL_String& src) { Construct(src.c_str(), src.length()); }
	~ZL_JSON_Impl() { Flags &= ~FLAG_ISROOT; KeepStringsIfReferenced(); ResetValue(); if (Flags & FLAG_KEYNEEDFREE) { free(ObjectKey); } }
	static ZL_JSON_Impl* GetProxyTarget(ZL_JSON_Impl* impl) { while (impl && impl->Type == TYPE_PROXY) { impl = impl->DataProxy; } return impl; }

	ZL_JSON_Impl* AddChild()
	{
		ZL_JSON_Impl* Object = new ZL_JSON_Impl;
		ZL_ASSERT(Type == ZL_Json::TYPE_ARRAY || Type == ZL_Json::TYPE_OBJECT);
		if (!DataChildren) DataChildren = new std::vector<ZL_JSON_Impl*>();
		DataChildren->push_back(Object);
		return DataChildren->back();
	}

	ZL_JSON_Impl* FindChild(const char* key, bool ResolveProxyTarget)
	{
		ZL_ASSERT(key);
		if (!key || Type != ZL_Json::TYPE_OBJECT || !DataChildren) return NULL;
		unsigned short KeyLen = (unsigned short)strlen(key);
		for (std::vector<ZL_JSON_Impl*>::iterator it = DataChildren->begin(); it != DataChildren->end(); ++it)
			if ((*it)->ObjectKeyLen == KeyLen && !strcmp((*it)->ObjectKey, key)) return (ResolveProxyTarget ? GetProxyTarget(*it) : *it);
		return NULL;
	}

	ZL_JSON_Impl* FindChild(size_t index, bool ResolveProxyTarget)
	{
		if ((Type != ZL_Json::TYPE_OBJECT && Type != ZL_Json::TYPE_ARRAY) || !DataChildren || index >= DataChildren->size()) return NULL;
		ZL_JSON_Impl* it = DataChildren->operator[](index);
		return (ResolveProxyTarget ? GetProxyTarget(it) : it);
	}

	void ToString(ZL_String& out, bool pretty_print, int indent = 0)
	{
		if (Type == TYPE_PROXY) DataProxy->ToString(out, pretty_print, indent);
		else if (Type == ZL_Json::TYPE_OBJECT)
		{
			if (!DataChildren || DataChildren->empty()) { out << '{' << '}';  return; }
			bool pp = (pretty_print && DataChildren && DataChildren->size() > 0);
			ZL_String nltabs; if (pp) { nltabs << '\n'; for (int i = 0; i < indent; i++) nltabs << '\t'; }
			out << '{';
			for (std::vector<ZL_JSON_Impl*>::iterator it = DataChildren->begin(); it != DataChildren->end(); ++it)
				{ if (it != DataChildren->begin()) out << ','; if (pp) out << nltabs << '\t'; else out << ' '; AppendJsonString(out, (*it)->ObjectKey); out << " : "; (*it)->ToString(out, pretty_print, indent+pp); }
			if (pp) out << nltabs;
			out << '}';
		}
		else if (Type == ZL_Json::TYPE_ARRAY)
		{
			out << "[";
			if (DataChildren) for (std::vector<ZL_JSON_Impl*>::iterator it = DataChildren->begin(); it != DataChildren->end(); ++it)
				{ if (it != DataChildren->begin()) out << ','; out << ' '; (*it)->ToString(out, pretty_print, indent); }
			out << " ]";
		}
		else if (Type == ZL_Json::TYPE_STRING) AppendJsonString(out, DataString);
		else if (Type == ZL_Json::TYPE_NUMBER) out << DataNumber;
		else out << (Type == ZL_Json::TYPE_TRUE ? "true" : (Type == ZL_Json::TYPE_FALSE ? "false" : "null"));
	}

	bool CheckIfHasChild(ZL_JSON_Impl* other)
	{
		if (this == other) return true;
		if (Type == TYPE_PROXY) return DataProxy->CheckIfHasChild(other);
		else if ((Type == ZL_Json::TYPE_OBJECT || Type == ZL_Json::TYPE_ARRAY) && DataChildren)
			for (std::vector<ZL_JSON_Impl*>::iterator it = DataChildren->begin(); it != DataChildren->end(); ++it) if ((*it)->CheckIfHasChild(other)) return true;
		return false;
	}

	void ResetValue(char NewType = ZL_Json::TYPE_NULL)
	{
		if ((Type == ZL_Json::TYPE_OBJECT || Type == ZL_Json::TYPE_ARRAY) && DataChildren) DeleteChildren();
		else if (Type == TYPE_PROXY) DataProxy->DelRef();
		else if (Type == ZL_Json::TYPE_STRING && (Flags & FLAG_STRINGNEEDFREE)) free(DataString);
		Type = NewType; DataChildren = NULL;
	}

private:
	void Construct(const char* src, size_t srclen)
	{
		if (!src || !*src) { Type = ZL_Json::TYPE_NULL; TypeAndFlags = 0; ObjectKey = NULL; DataChildren = NULL; return; }
		Type = TYPE_PROXY, Flags = FLAG_KEYNEEDFREE|FLAG_ISROOT, ObjectKey = (char*)malloc(srclen + 1), ObjectKeyLen = 0, DataProxy = NULL;
		memcpy(ObjectKey, src, srclen); ObjectKey[srclen] = '\0';
		char *end = ObjectKey + srclen, *parseend = Parse(EndOfWhitespace(ObjectKey), end);
		if (Type != PARSE_ERROR && EndOfWhitespace(parseend) == end) return; //success

		ZL_LOG2("JSON", "Parsing Error - Offset: %d - Error At: %.100s\n", parseend - ObjectKey, parseend);
		free(ObjectKey); ObjectKey = NULL; Flags &= ~FLAG_KEYNEEDFREE; ResetValue(); //cleanup
	}

	void KeepStringsIfReferenced(bool parent_is_referenced = false)
	{
		if (!ZL_VERIFY(!(Flags & FLAG_ISROOT))) return; //other separately allocated root trees can only be referenced by proxy
		if ((Type == ZL_Json::TYPE_OBJECT || Type == ZL_Json::TYPE_ARRAY) && DataChildren)
			for (std::vector<ZL_JSON_Impl*>::iterator it = DataChildren->begin(); it != DataChildren->end(); ++it)
				(*it)->KeepStringsIfReferenced(GetRefCount() > 1);
		if (!parent_is_referenced && GetRefCount() <= 1) return;
		if (ObjectKey && !(Flags & FLAG_KEYNEEDFREE))                       { Flags |= FLAG_KEYNEEDFREE;    char* src = ObjectKey;  size_t ln = strlen(src); memcpy((ObjectKey  = (char*)malloc(ln + 1)), src, ln + 1); }
		if (Type == ZL_Json::TYPE_STRING && !(Flags & FLAG_STRINGNEEDFREE)) { Flags |= FLAG_STRINGNEEDFREE; char* src = DataString; size_t ln = strlen(src); memcpy((DataString = (char*)malloc(ln + 1)), src, ln + 1); }
	}

	void AppendJsonString(ZL_String& out, char* jstr)
	{
		for (out << '"'; *jstr; jstr++)
			if (*jstr == '\\' || *jstr == '"') out << '\\' << *jstr;
			else if (*jstr == '\b') out << '\\' <<  'b'; else if (*jstr == '\f') out << '\\' <<  'f';
			else if (*jstr == '\n') out << '\\' <<  'n'; else if (*jstr == '\r') out << '\\' <<  'r';
			else if (*jstr == '\t') out << '\\' <<  't'; else out << *jstr;
		out << '"';
	}

	char* Parse(char* p, char *end)
	{
		#define CLC(c) (c|0x60) //char to lower case
		if ((*p == 'N' || *p == 'n') && (end-p) >= 4 && CLC(p[1]) == 'u' && CLC(p[2]) == 'l' && CLC(p[3]) == 'l') { Type = ZL_Json::TYPE_NULL; return p + 4; }
		if ((*p == 'T' || *p == 't') && (end-p) >= 4 && CLC(p[1]) == 'r' && CLC(p[2]) == 'u' && CLC(p[3]) == 'e') { Type = ZL_Json::TYPE_TRUE; return p + 4; }
		if ((*p == 'F' || *p == 'f') && (end-p) >= 5 && CLC(p[1]) == 'a' && CLC(p[2]) == 'l' && CLC(p[3]) == 's' && CLC(p[4]) == 'e') { Type = ZL_Json::TYPE_FALSE; return p + 5; }
		#undef CLC
		if ((*p >= '0' && *p <= '9') || *p == '-')
		{
			Type = ZL_Json::TYPE_NUMBER;
			char *AfterNumber = NULL;
			DataNumber = (scalar)strtod(p, &AfterNumber);
			if (!AfterNumber || AfterNumber == p) { Type = PARSE_ERROR; return p; }
			return AfterNumber;
		}
		if (*p == '"')
		{
			DataString = ++p;
			char* trg = NULL;
			for (; p < end; p++)
			{
				if (*p == '\\')
				{
					if (!trg) trg = p;
					if (p[1] == '\\' || p[1] == '"' || p[1] == '/') *trg = p[1];
					else if (p[1] == 'b') *trg = '\b';
					else if (p[1] == 'f') *trg = '\f';
					else if (p[1] == 'n') *trg = '\n';
					else if (p[1] == 'r') *trg = '\r';
					else if (p[1] == 't') *trg = '\t';
					else break;
					p++;
					trg++;
				}
				else if (*p == '"') { *(trg ? trg : p) = '\0'; Type = ZL_Json::TYPE_STRING; return p+1; }
				else if (trg) *(trg++) = *p;
			}
			Type = PARSE_ERROR;
			return p;
		}
		const char ListEndChar = (*p == '[' ? ']' : (*p == '{' ?  '}' : '\0'));
		if (!ListEndChar) { Type = PARSE_ERROR; return p; }

		p = EndOfWhitespace(p+1);
		if (*p == ListEndChar) { Type = (ListEndChar == ']' ? ZL_Json::TYPE_ARRAY : ZL_Json::TYPE_OBJECT); DataChildren = NULL; return p+1; }
		ZL_JSON_Impl* Child;
		for (DataChildren = new std::vector<ZL_JSON_Impl*>(), Type = PARSE_ERROR; (Child = new ZL_JSON_Impl); p = EndOfWhitespace(p+1))
		{
			if (ListEndChar == '}')
			{
				p = Child->Parse(p, end);
				if (Child->Type != ZL_Json::TYPE_STRING) break;
				Child->ObjectKey = Child->DataString;
				Child->ObjectKeyLen = (unsigned short)strlen(Child->ObjectKey); //can't just count bytes consumed by parse due to escaped chars

				p = EndOfWhitespace(p);
				if (*p != ':') break;
				p = EndOfWhitespace(p+1);
			}

			p = Child->Parse(p, end);
			if (Child->Type == PARSE_ERROR) break;
			DataChildren->push_back(Child);
			Child = NULL;

			p = EndOfWhitespace(p);
			if (*p == ListEndChar) { Type = (ListEndChar == ']' ? ZL_Json::TYPE_ARRAY : ZL_Json::TYPE_OBJECT); break; }
			else if (*p != ',') break;
		}
		if (Type == PARSE_ERROR) { if (Child) delete Child; DeleteChildren(); return p; }
		return p+1;
	}
	void DeleteChildren() { for (std::vector<ZL_JSON_Impl*>::iterator it = DataChildren->begin(); it != DataChildren->end(); ++it) (*it)->DelRef(); delete DataChildren; }
	char* EndOfWhitespace(char* p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
};

ZL_IMPL_OWNER_NOASSIGNMENT_IMPLEMENTATIONS(ZL_Json)
ZL_Json::ZL_Json(const char *json) : impl(new ZL_JSON_Impl(json)) {}
ZL_Json::ZL_Json(const char *json, size_t len) : impl(new ZL_JSON_Impl(json, len)) {}
ZL_Json::ZL_Json(const ZL_String &json) : impl(new ZL_JSON_Impl(json)) {}
ZL_Json::ZL_Json(const ZL_File &file) : impl(new ZL_JSON_Impl(file.GetContents())) {}
ZL_Json::ZL_Json(ZL_JSON_Impl *fromimpl) : impl(fromimpl) { if (impl) impl->AddRef(); }

ZL_Json::eType ZL_Json::GetType() const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp ? (ZL_Json::eType)imp->Type : TYPE_NULL);
}

size_t ZL_Json::Size() const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp && imp->DataChildren && (imp->Type == TYPE_OBJECT || imp->Type == TYPE_ARRAY) ? imp->DataChildren->size() : 0);
}

ZL_Json::Iterator ZL_Json::GetIterator() const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	ZL_Json::Iterator ret;
	if (!imp || !imp->DataChildren || imp->DataChildren->empty() || (imp->Type != TYPE_OBJECT && imp->Type != TYPE_ARRAY)) memset(&ret, 0, sizeof(ret));
	else { ret.cursor = ret.begin = &*imp->DataChildren->begin(); ret.end = ret.cursor + imp->DataChildren->size(); }
	return ret;
}

std::vector<ZL_Json> ZL_Json::GetChildren() const
{
	std::vector<ZL_Json> ret;
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp && imp->DataChildren && (imp->Type == TYPE_OBJECT || imp->Type == TYPE_ARRAY))
		for (std::vector<ZL_JSON_Impl*>::iterator it = imp->DataChildren->begin(); it != imp->DataChildren->end(); ++it)
			ret.push_back(ZL_Json(*it));
	return ret;
}

ZL_Json ZL_Json::GetChild(size_t index) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return ZL_Json(imp ? imp->FindChild(index, false) : NULL);
}

ZL_Json ZL_Json::GetChildOf(const char* child_key, size_t index) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_key, true);
	return ZL_Json(imp ? imp->FindChild(index, false) : NULL);
}

ZL_Json ZL_Json::GetByKey(const char* key) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return ZL_Json(imp ? imp->FindChild(key, false) : NULL);
}

const char* ZL_Json::GetString(const char* default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp && imp->Type == TYPE_STRING ? imp->DataString : default_value);
}

scalar ZL_Json::GetFloat(scalar default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp && imp->Type == TYPE_NUMBER ? (scalar)imp->DataNumber : default_value);
}

int ZL_Json::GetInt(int default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp && imp->Type == TYPE_NUMBER ? (int)imp->DataNumber : default_value);
}

bool ZL_Json::GetBool() const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp && imp->Type == TYPE_TRUE);
}

bool ZL_Json::IsNull() const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (!imp || imp->Type == TYPE_NULL);
}

bool ZL_Json::HasKey(const char* key) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	return (imp && imp->FindChild(key, false));
}

const char* ZL_Json::GetKey() const
{
	return (impl ? impl->ObjectKey : NULL);
}

const char* ZL_Json::GetStringOf(const char* child_key, const char* default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_key, true);
	return (imp && imp->Type == TYPE_STRING ? imp->DataString : default_value);
}

const char* ZL_Json::GetStringOf(size_t child_index, const char* default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_index, true);
	return (imp && imp->Type == TYPE_STRING ? imp->DataString : default_value);
}

scalar ZL_Json::GetFloatOf(const char* child_key, scalar default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_key, true);
	return (imp && imp->Type == TYPE_NUMBER ? (scalar)imp->DataNumber : default_value);
}

scalar ZL_Json::GetFloatOf(size_t child_index, scalar default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_index, true);
	return (imp && imp->Type == TYPE_NUMBER ? (scalar)imp->DataNumber : default_value);
}

int ZL_Json::GetIntOf(const char* child_key, int default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_key, true);
	return (imp && imp->Type == TYPE_NUMBER ? (int)imp->DataNumber : default_value);
}

int ZL_Json::GetIntOf(size_t child_index, int default_value) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_index, true);
	return (imp && imp->Type == TYPE_NUMBER ? (int)imp->DataNumber : default_value);
}

bool ZL_Json::GetBoolOf(const char* child_key) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_key, true);
	return (imp && imp->Type == TYPE_TRUE);
}

bool ZL_Json::GetBoolOf(size_t child_index) const
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp) imp = imp->FindChild(child_index, true);
	return (imp && imp->Type == TYPE_TRUE);
}

ZL_Json ZL_Json::operator[](const char* key)
{
	ZL_JSON_Impl *imp = (impl ? ZL_JSON_Impl::GetProxyTarget(impl) : (impl = new ZL_JSON_Impl));
	if (imp->Type == TYPE_NULL) imp->ResetValue(TYPE_OBJECT);
	if (imp->Type != TYPE_OBJECT || !key) return ZL_Json();
	ZL_JSON_Impl* child = imp->FindChild(key, false);
	if (!child)
	{
		child = imp->AddChild();
		child->Flags |= ZL_JSON_Impl::FLAG_KEYNEEDFREE;
		size_t len = strlen(key);
		memcpy((child->ObjectKey = (char*)malloc(len + 1)), key, len + 1);
		child->ObjectKeyLen = (unsigned short)len;
	}
	return ZL_Json(child);
}

ZL_Json ZL_Json::Add()
{
	ZL_JSON_Impl *imp = (impl ? ZL_JSON_Impl::GetProxyTarget(impl) : (impl = new ZL_JSON_Impl));
	if (imp->Type == TYPE_NULL) imp->ResetValue(TYPE_ARRAY);
	if (imp->Type != TYPE_ARRAY) return ZL_Json();
	return ZL_Json(imp->AddChild());
}

void ZL_Json::SetString(const char* NewString)
{
	if (!NewString) { SetNull(); return; }
	ZL_JSON_Impl *imp = (impl ? impl : (impl = new ZL_JSON_Impl));
	imp->ResetValue(TYPE_STRING);
	imp->Flags |= ZL_JSON_Impl::FLAG_STRINGNEEDFREE;
	size_t len = strlen(NewString);
	memcpy((imp->DataString = (char*)malloc(len + 1)), NewString, len + 1);
}

void ZL_Json::SetFloat(scalar NewFloat)
{
	ZL_JSON_Impl *imp = (impl ? impl : (impl = new ZL_JSON_Impl));
	imp->ResetValue(TYPE_NUMBER);
	imp->DataNumber = NewFloat;
}

void ZL_Json::SetInt(int NewInt)
{
	ZL_JSON_Impl *imp = (impl ? impl : (impl = new ZL_JSON_Impl));
	imp->ResetValue(TYPE_NUMBER);
	imp->DataNumber = (scalar)NewInt;
}

void ZL_Json::SetBool(bool NewBool)
{
	(impl ? impl : (impl = new ZL_JSON_Impl))->ResetValue(NewBool ? TYPE_TRUE : TYPE_FALSE);
}

void ZL_Json::SetNull()
{
	(impl ? impl : (impl = new ZL_JSON_Impl))->ResetValue();
}

void ZL_Json::SetReference(const ZL_Json &source)
{
	if (!impl || (impl->Flags & ZL_JSON_Impl::FLAG_ISROOT)) { ZL_Impl::CopyRef(source.impl, (ZL_Impl*&)impl); return; }
	if (!source.impl || source.impl->CheckIfHasChild(impl)) { SetNull(); return; }
	impl->ResetValue(ZL_JSON_Impl::TYPE_PROXY);
	impl->DataProxy = source.impl;
	impl->DataProxy->AddRef();
}

void ZL_Json::SetKey(const char* NewKey)
{
	if (!impl || (impl->Flags & ZL_JSON_Impl::FLAG_ISROOT)) return;
	if (impl->Flags & ZL_JSON_Impl::FLAG_KEYNEEDFREE) free(impl->ObjectKey);
	impl->Flags |= ZL_JSON_Impl::FLAG_KEYNEEDFREE;
	size_t len = strlen(NewKey);
	memcpy((impl->ObjectKey = (char*)malloc(len + 1)), NewKey, len + 1);
	impl->ObjectKeyLen = (unsigned short)len;
}

ZL_Json::Iterator ZL_Json::Erase(Iterator it)
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (!imp || !imp->DataChildren || imp->DataChildren->empty() || (imp->Type != TYPE_OBJECT && imp->Type != TYPE_ARRAY) ||
		it.cursor < &*imp->DataChildren->begin() || it.cursor >= &*imp->DataChildren->begin()+imp->DataChildren->size()) { memset(&it, 0, sizeof(it)); return it; }
	(*(it.cursor))->DelRef();
	imp->DataChildren->erase(imp->DataChildren->begin() + (it.cursor - it.begin));
	it.end--;
	return it;
}

bool ZL_Json::Erase(const ZL_Json &child)
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl), *childimpl = child.impl, *childimp = ZL_JSON_Impl::GetProxyTarget(childimpl);
	if (imp && imp->DataChildren && (imp->Type == TYPE_OBJECT || imp->Type == TYPE_ARRAY))
		for (std::vector<ZL_JSON_Impl*>::iterator it = imp->DataChildren->begin(); it != imp->DataChildren->end(); ++it)
			if (*it == childimpl || *it == childimp) { (*it)->DelRef(); imp->DataChildren->erase(it); return true; }
	return false;
}

bool ZL_Json::Erase(const char* key)
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (!imp || !key || imp->Type != ZL_Json::TYPE_OBJECT || !imp->DataChildren) return false;
	unsigned short KeyLen = (unsigned short)strlen(key);
	for (std::vector<ZL_JSON_Impl*>::iterator it = imp->DataChildren->begin(); it != imp->DataChildren->end(); ++it)
		if ((*it)->ObjectKeyLen == KeyLen && !strcmp((*it)->ObjectKey, key)) { (*it)->DelRef(); imp->DataChildren->erase(it); return true; }
	return false;
}

bool ZL_Json::EraseAt(size_t index)
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp && imp->DataChildren && (imp->Type == TYPE_OBJECT || imp->Type == TYPE_ARRAY) && index < imp->DataChildren->size())
		{ (*imp->DataChildren->operator[](index)).DelRef(); imp->DataChildren->erase(imp->DataChildren->begin() + index); return true; }
	return false;
}

void ZL_Json::Clear()
{
	ZL_JSON_Impl *imp = ZL_JSON_Impl::GetProxyTarget(impl);
	if (imp && (imp->Type == TYPE_OBJECT || imp->Type == TYPE_ARRAY)) imp->ResetValue(imp->Type);
}

ZL_String ZL_Json::ToString(bool pretty_print) const
{
	ZL_String ret;
	if (impl) impl->ToString(ret, pretty_print);
	return ret;
}

struct ZL_Xml_Impl : public ZL_Impl
{
	const ZL_Xml_Impl *parent;
	std::vector<ZL_Xml_Impl*> children;
	std::map<ZL_String, ZL_String> parameters;
	ZL_String name, text;

	ZL_Xml_Impl(const ZL_Xml_Impl *parent) : parent(parent) { }

	ZL_Xml_Impl(const ZL_Xml_Impl *parent, const ZL_String &name) : parent(parent), name(name) { }

	~ZL_Xml_Impl()
	{
		for (std::vector<ZL_Xml_Impl*>::iterator itc = children.begin(); itc != children.end(); ++itc)
			(*itc)->DelRef();
	}

	bool hasparent(const ZL_Xml_Impl *impl)
	{
		if (!impl || !parent) return false;
		for (const ZL_Xml_Impl *p = parent; p; p = p->parent) if (p == impl) return true;
		return false;
	}

	ZL_Xml_Impl(const ZL_Xml_Impl *parent, ZL_Xml_Impl* clone) : parent(parent)
	{
		if (!clone) return;
		name = clone->name;
		text = clone->text;
		parameters = clone->parameters;
		for (std::vector<ZL_Xml_Impl*>::iterator itc = clone->children.begin(); itc != clone->children.end(); ++itc)
			children.push_back(new ZL_Xml_Impl(this, *itc));
	}

	ZL_Xml_Impl(const ZL_Xml_Impl *parent, const ZL_String &xml, const ZL_String::size_type from, const ZL_String::size_type to) : parent(parent)
	{
		// cursor, open tag <, close tag >, name start, name end, parameter start, parameter end, equal sign, valua start, value end, child from
		ZL_String::size_type cur = from, op, cl, nen = 0, pst = 0, pen = 0, veq = 0, vst = 0, cfrom = 0;
		const ZL_String::chr *p, *pxml = xml.c_str();
		bool terminated = false, closing;

		xml_next_tag:
		op = xml.find("<", cur);
		if (op == ZL_String::npos || op > to) return;
		cur = op + 1;
		p = &pxml[cur];

		if (from == 0)
		{
			if      (to-cur >= 4 &&*p=='!' &&*(p+1)=='-'&&*(p+2)=='-' ) { for (; cur < to; cur++, p++) if (*p=='>'&&*(p-2)=='-'&&*(p-1)=='-') goto xml_next_tag; }
			else if (to-cur >= 2 &&*p=='?')                             { for (; cur < to; cur++, p++) if (*p=='>'&&*(p-1)=='?')              goto xml_next_tag; }
		}

		cl = xml.find(">", cur);
		if (cl == ZL_String::npos || cl > to || cl < op + 2) return;

		#define ADD_PARAM_VAL  { parameters[xml.substr(pst, pen - pst)] = xml.substr(vst, cur - vst); pst = cur+1; vst = veq = pen = 0; }
		#define ADD_PARAM_FLAG { parameters[xml.substr(pst, pen - pst)] = ZL_String(); pst = cur; pen = 0; }

		for (; cur <= cl; cur++, p++)
		{
			if (*p==' '||*p=='='||*p=='"'||*p=='\''||*p=='/'||*p=='\t'||*p=='\r'||*p=='\n'||*p=='<'||*p=='>')
			{
				if (nen == 0) { nen = cur; name = xml.substr(op+1, cur - op - 1); }
				if (vst == 0 && !terminated && *p == '/') terminated = true;
				if (pst == 0 || pst == cur) { pst = cur+1; continue; }
				if (terminated) terminated = false;
				if (pen == 0) { pen = cur; }
				if (veq == 0) { if (*p == '=') { veq = cur; vst = cur+1; } continue; }
				if (vst == 0 || (vst == cur && pxml[vst-1]!='"' && pxml[vst-1]!='\'')) { vst = cur+1; continue; }
				if (pxml[vst-1]=='"'||pxml[vst-1]=='\'')
				{
					if (*p=='>') { if ((cl = xml.find(">", cur+1)) == ZL_String::npos || cl > to || cl < cur + 2) break; }
					else if (pxml[vst-1]==*p) ADD_PARAM_VAL
				}
				else ADD_PARAM_VAL
			}
			else if (pen && veq == 0) ADD_PARAM_FLAG
			else if (terminated) terminated = false;
		}
		if (pen && veq == 0) ADD_PARAM_FLAG

		for (std::map<ZL_String, ZL_String>::iterator itp = parameters.begin(); itp != parameters.end(); ++itp)
			ConvertFromEntities(itp->second);

		if (terminated) return;

		op = cl = 0; terminated = closing = false;

		int level = 0;
		for (; cur < to; cur++, p++)
		{
			if (op == 0)
			{
				if      (*p == '<' && to-cur >= 4 &&*(p+1)=='!' &&*(p+2)=='-'&&*(p+3)=='-' )            { for (            ; cur < to; cur++, p++) if (*p=='>'&&*(p-2)=='-'&&*(p-1)=='-') break; }
				else if (*p == '<' && to-cur >= 2 &&*(p+1)=='?')                                        { for (            ; cur < to; cur++, p++) if (*p=='>'&&*(p-1)=='?')              break; }
				else if (*p == '<' && to-cur >= 9 &&*(p+1)=='!' &&*(p+2)=='['&&!memcmp(p+3,"CDATA[",6)) { for (op = cur + 9; cur < to; cur++, p++) if (*p=='>'&&*(p-2)==']'&&*(p-1)==']') { if (level == 0) text += ZL_String::str_replace(xml.substr(op, cur-2-op), "&", "&amp;"); op = 0; break; } }
				else if (*p == '<') { if (level++ == 0) { cfrom = cur; } op = cur; cl = 0; }
				else if (level == 0 && (text.length() || *p > ' ')) { text += *p; }
			}
			else
			{
				if (*p=='/') { if (cl == 0) { level--; closing = true; } terminated = true; }
				else if (*p > ' ') { cl = cur; if (terminated && *p!='>') terminated = false; }

				if      (vst == 0 && (*p=='"'||*p=='\'')) { vst = cur+1; }
				else if (vst != 0 && pxml[vst-1]==*p) { vst = 0; }
				else if (vst == 0 && *p=='>')
				{
					if ((closing || terminated) && (level == 0)) break;
					if ((closing || terminated) && (--level == 0)) children.push_back(new ZL_Xml_Impl(this, xml, cfrom, cur+1));
					op = cl = 0; terminated = closing = false;
				}
			}
		}

		while (text.length()) { if (*text.rbegin()<=' ') text.erase(text.end()-1); else break; }
		ConvertFromEntities(text);
	}

	void ToString(ZL_String &xml, int iteration_level = 0)
	{
		ZL_String vtmp;
		if (iteration_level) xml += ZL_String('\t') * iteration_level;
		//xml << '<' << (int)this << '_' << iRefCount << '_' << name;
		xml << '<' << name;

		for (std::map<ZL_String, ZL_String>::iterator itp = parameters.begin(); itp != parameters.end(); ++itp)
			xml << ' ' << itp->first << "=\"" << ConvertToEntities(itp->second, vtmp) << '"';

		xml += (children.size() || text.size() ? ">\n" : (parameters.size() ? " />\n" : "/>\n"));

		if (text.size()) xml << ZL_String('\t') * (iteration_level+1) << ConvertToEntities(text, vtmp) << '\n';

		for (std::vector<ZL_Xml_Impl*>::iterator itc = children.begin(); itc != children.end(); ++itc)
			(*itc)->ToString(xml, iteration_level + 1);

		if (children.size() || text.size()) xml << ZL_String('\t') * iteration_level << "</" << name << ">\n";
	}

	static bool CompareLowerCase(const char* pCharsAnyCase, const char* pCompareValLower, int pCompareValLen)
	{
		while (pCompareValLen--) if (tolower(*pCharsAnyCase++) != *pCompareValLower++) return false;
		return true;
	}

	static void ConvertFromEntities(ZL_String &v)
	{
		for (ZL_String::size_type p = 0; p < v.size(); p++)
			if (v[p] != '&') { }
			else if (v[p+3] == ';' && CompareLowerCase(&v[p+1], "lt",   2)) v.replace(p, 4, "<");
			else if (v[p+3] == ';' && CompareLowerCase(&v[p+1], "gt",   2)) v.replace(p, 4, ">");
			else if (v[p+5] == ';' && CompareLowerCase(&v[p+1], "quot", 4)) v.replace(p, 6, "\"");
			else if (v[p+5] == ';' && CompareLowerCase(&v[p+1], "apos", 4)) v.replace(p, 6, "\'");
			else if (v[p+4] == ';' && CompareLowerCase(&v[p+1], "amp",  3)) v.replace(p, 5, "&");
	}

	static ZL_String & ConvertToEntities(const ZL_String &vsource, ZL_String &v)
	{
		v = vsource; for (ZL_String::size_type p = 0; p < v.size(); p++)
			if      (v[p] == '<')  { v.replace(p, 1, "&lt;");   p += 3; }
			else if (v[p] == '>')  { v.replace(p, 1, "&gt;");   p += 3; }
			else if (v[p] == '"')  { v.replace(p, 1, "&quot;"); p += 5; }
			else if (v[p] == '\'') { v.replace(p, 1, "&apos;"); p += 5; }
			else if (v[p] == '&')  { v.replace(p, 1, "&amp;");  p += 4; }
		return v;
	}
};

ZL_IMPL_OWNER_DEFAULT_IMPLEMENTATIONS(ZL_Xml)
ZL_Xml::ZL_Xml(const ZL_File &file) : impl(new ZL_Xml_Impl(NULL, file.GetContents(), 0, file.Size())) { }
ZL_Xml::ZL_Xml(const ZL_String &xml) : impl(new ZL_Xml_Impl(NULL, xml, 0, xml.size())) { }
ZL_Xml::ZL_Xml(ZL_Xml_Impl *fromimpl) : impl(fromimpl) { impl->AddRef(); }

ZL_Xml ZL_Xml::AddChild(ZL_Xml &Child)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	//else if (impl == Child.impl || impl->hasparent(Child.impl)) return ZL_Xml();
	ZL_Xml_Impl *newchild = new ZL_Xml_Impl(impl, Child.impl);
	impl->children.push_back(newchild);
	return ZL_Xml(newchild);
}

void ZL_Xml::AddChild(ZL_Xml *pChild)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	if (!pChild || impl->hasparent(pChild->impl)) return;
	ZL_Xml_Impl *newchild;
	if (!pChild->impl) { newchild = pChild->impl = new ZL_Xml_Impl(impl); newchild->AddRef(); }
	else if (pChild->impl->parent == NULL) { newchild = pChild->impl; newchild->parent = impl; newchild->AddRef(); }
	else newchild = new ZL_Xml_Impl(impl, pChild->impl);
	impl->children.push_back(newchild);
}

ZL_Xml ZL_Xml::AddChild(const ZL_String& name)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	ZL_Xml_Impl *newchild = new ZL_Xml_Impl(impl, name);
	impl->children.push_back(newchild);
	return ZL_Xml(newchild);
}

void ZL_Xml::RemoveChild(const ZL_Xml *pChild)
{
	if (!impl || !pChild->impl) return;
	for (std::vector<ZL_Xml_Impl*>::iterator itc = impl->children.begin(); itc != impl->children.end(); ++itc)
		if ((*itc) == pChild->impl) { (*itc)->DelRef(); impl->children.erase(itc); return; }
}

void ZL_Xml::RemoveChildren(const ZL_String& name)
{
	if (!impl) return;
	for (std::vector<ZL_Xml_Impl*>::iterator itc = impl->children.begin(); itc != impl->children.end();)
		if ((*itc)->name == name) { (*itc)->DelRef(); itc = impl->children.erase(itc); return; } else ++itc;
}

ZL_String ZL_Xml::ToString() const
{
	ZL_String s;
	if (impl) impl->ToString(s, 0);
	return s;
}

const ZL_String& ZL_Xml::GetName() const
{
	if (impl) return impl->name;
	return ZL_String::EmptyString;
}

void ZL_Xml::SetName(const ZL_String& newname)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	impl->name = newname;
}

const ZL_String& ZL_Xml::GetText() const
{
	if (impl) return impl->text;
	return ZL_String::EmptyString;
}

void ZL_Xml::SetText(const ZL_String& newtext)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	impl->text = newtext;
}

void ZL_Xml::SetParameter(const ZL_String &name, const ZL_String &paramval)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	impl->parameters[name] = paramval;
}

void ZL_Xml::RemoveParameter(const ZL_String &name)
{
	if (!impl) return;
	impl->parameters.erase(name);
}

bool ZL_Xml::HasParameter(const ZL_String &name) const
{
	if (!impl) return false;
	return (impl->parameters.count(name) ? true : false);
}

std::map<ZL_String, ZL_String> &ZL_Xml::GetParameters()
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	return impl->parameters;
}

std::vector<ZL_Xml> ZL_Xml::GetChildren() const
{
	std::vector<ZL_Xml> children;
	if (!impl) return children;
	for (std::vector<ZL_Xml_Impl*>::iterator itc = impl->children.begin(); itc != impl->children.end(); ++itc)
		children.push_back(ZL_Xml(*itc));
	return children;
}

std::vector<ZL_Xml> ZL_Xml::GetChildrenByName(const ZL_String &name) const
{
	std::vector<ZL_Xml> children;
	if (!impl) return children;
	for (std::vector<ZL_Xml_Impl*>::iterator itc = impl->children.begin(); itc != impl->children.end(); ++itc)
		if ((*itc)->name == name) children.push_back(ZL_Xml(*itc));
	return children;
}

ZL_String& ZL_Xml::operator[](const ZL_String &paramname) const
{
	if (!impl) return ZL_String::EmptyString;
	std::map<ZL_String, ZL_String>::iterator it = impl->parameters.find(paramname);
	if (it == impl->parameters.end()) return ZL_String::EmptyString;
	return it->second;
}

std::allocator<ZL_String>::reference ZL_Xml::operator[](const ZL_String &paramname)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	return impl->parameters[paramname];
}

ZL_Xml ZL_Xml::operator()(const ZL_String &childname)
{
	if (!impl) impl = new ZL_Xml_Impl(NULL);
	for (std::vector<ZL_Xml_Impl*>::iterator itc = impl->children.begin(); itc != impl->children.end(); ++itc)
		if ((*itc)->name == childname) return ZL_Xml(*itc);
	return ZL_Xml();
}

static const char base64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char base64pad = '=';

ZL_String ZL_Base64::Encode(const void* InputBuffer, size_t BufSize)
{
	if (BufSize == 0) return "";
	unsigned int n = 0;
	unsigned char n0, n1, n2, n3;
	ZL_String res;
	const unsigned char *p = (const unsigned char*)InputBuffer;
	for (size_t x = 0; x < BufSize; x += 3)
	{
		n = p[x] << 16;
		if ((x + 1) < BufSize) n += p[x + 1] << 8;
		if ((x + 2) < BufSize) n += p[x + 2];
		n0 = (unsigned char)(n >> 18) & 63;
		n1 = (unsigned char)(n >> 12) & 63;
		n2 = (unsigned char)(n >> 6) & 63;
		n3 = (unsigned char)n & 63;
		res << base64chars[n0];
		res << base64chars[n1];
		if ((x + 1) < BufSize) res << base64chars[n2];
		if ((x + 2) < BufSize) res << base64chars[n3];
	}
	for (size_t pad_count = 0; pad_count < res.size() % 4; pad_count++) res << base64pad;
	return res;
}

size_t ZL_Base64::Decode(const ZL_String& Base64Data, std::vector<unsigned char> &output)
{
	unsigned char DecodeMatrix[256];
	memset(DecodeMatrix, 0, 256);
	for (unsigned char im = 0; im <= 63; im++) DecodeMatrix[(int)base64chars[(int)im]] = im;
	size_t iBuf = 0, total = Base64Data.size();
	output = std::vector<unsigned char>(DecodedMaxSize(total));
	unsigned char* pOutput = &output[0];
	unsigned char* pText = (unsigned char*)Base64Data.c_str();
	for (size_t i = 0, left = total; i < total; left -= 4, i += 4)
	{
		pOutput[iBuf] = DecodeMatrix[pText[i]] << 2;
		if (left == 1 || pText[i + 1] == base64pad) break;
		pOutput[iBuf++] += (DecodeMatrix[pText[i + 1]] >> 4) & 0x3;
		pOutput[iBuf] = DecodeMatrix[pText[i + 1]] << 4;
		if (left == 2 || pText[i + 2] == base64pad) break;
		pOutput[iBuf++] += (DecodeMatrix[pText[i + 2]] >> 2) & 0xf;
		pOutput[iBuf] = DecodeMatrix[pText[i + 2]] << 6;
		if (left == 3 || pText[i + 3] == base64pad) break;
		pOutput[iBuf++] += DecodeMatrix[pText[i + 3]];
	}
	if (iBuf != output.size()) output.erase(output.begin()+iBuf, output.end());
	return iBuf;
}

size_t ZL_Base64::Decode(const ZL_String& Base64Data, void* OutputBuffer, size_t BufSize)
{
	unsigned char DecodeMatrix[256];
	memset(DecodeMatrix, 0, 256);
	for (unsigned char im = 0; im <= 63; im++) DecodeMatrix[(int)base64chars[(int)im]] = im;
	size_t iBuf = 0, total = Base64Data.size();
	unsigned char* pOutput = (unsigned char*)OutputBuffer;
	unsigned char* pText = (unsigned char*)Base64Data.c_str();
	for (size_t i = 0, left = total; i < total && iBuf < BufSize; left -= 4, i += 4)
	{
		pOutput[iBuf] = DecodeMatrix[pText[i]] << 2;
		if (left == 1 || pText[i + 1] == base64pad) break;
		pOutput[iBuf++] += (DecodeMatrix[pText[i + 1]] >> 4) & 0x3;
		if (iBuf == BufSize) break;
		pOutput[iBuf] = DecodeMatrix[pText[i + 1]] << 4;
		if (left == 2 || pText[i + 2] == base64pad) break;
		pOutput[iBuf++] += (DecodeMatrix[pText[i + 2]] >> 2) & 0xf;
		if (iBuf == BufSize) break;
		pOutput[iBuf] = DecodeMatrix[pText[i + 2]] << 6;
		if (left == 3 || pText[i + 3] == base64pad) break;
		pOutput[iBuf++] += DecodeMatrix[pText[i + 3]];
	}
	return iBuf;
}

size_t ZL_Base64::DecodedMaxSize(size_t EncodedSize)
{
	return (EncodedSize + 3) / 4 * 3;
}

bool ZL_Base64::IsBase64(const ZL_String& Base64Data)
{
	unsigned char TestMatrix[256], *pText = (unsigned char*)Base64Data.c_str();
	memset(TestMatrix, 65, 256);
	for (unsigned char im = 0; im <= 63; im++) TestMatrix[(int)base64chars[(int)im]] = im;
	TestMatrix[(int)base64pad] = 64; //Padding character for test
	for (size_t i = 0; i < Base64Data.size(); i++)
	{
		if (TestMatrix[pText[i]] == 65) return false; //bad char
		if ((TestMatrix[pText[i]] == 64) && (i < Base64Data.size() - 3)) return false; //bad padding
	}
	return true;
}

#include "zlib/zlib.h"

size_t ZL_Compression::Compress(const void* InBuf, size_t InSize, std::vector<unsigned char> &out, int Level, bool ReserveMaxCompressSize)
{
	z_stream z;
	memset(&z, 0, sizeof(z));
	if (sizeof(InSize) != sizeof(z.total_in) && !ZL_VERIFY(InSize <= 0xFFFFFFFF)) return 0; //zlib limit
	if (deflateInit(&z, Level) != Z_OK) return 0;
	size_t head = out.size();
	if (ReserveMaxCompressSize) out.reserve(head + deflateBound(&z, (unsigned long)InSize));
	for (;;)
	{
		if (!z.avail_in && InSize) { z.next_in = (unsigned char*)InBuf + z.total_in; InSize -= (z.avail_in = (InSize < 1048576 ? (unsigned int)InSize : 1048576)); }
		if (!z.avail_out) { out.resize(out.size() + 1024); z.next_out = &out[head + z.total_out]; z.avail_out = (unsigned int)(out.size() - head - z.total_out); }
		int status = deflate(&z, (InSize ? Z_NO_FLUSH : Z_FINISH));
		if (status == Z_OK) continue;
		if (!ZL_VERIFY(status == Z_STREAM_END)) z.avail_out += z.total_out;
		break;
	}
	out.resize(out.size() - z.avail_out);
	ZL_ASSERT(deflateEnd(&z) == Z_OK);
	return out.size() - head;
}

size_t ZL_Compression::CompressMaxSize(size_t DecompressedSize)
{
	return compressBound((unsigned long)DecompressedSize);
}

bool ZL_Compression::Compress(const void* InBuffer, size_t InSize, const void* OutBuffer, size_t* OutSize, int Level)
{
	if (sizeof(InSize) != sizeof(unsigned long) && !ZL_VERIFY(InSize <= 0xFFFFFFFF)) return false; //zlib limit
	if (sizeof(*OutSize) != sizeof(unsigned long) && !ZL_VERIFY(*OutSize <= 0xFFFFFFFF)) *OutSize = 0xFFFFFFFF; //zlib limit
	unsigned long DestSize = (unsigned long)*OutSize;
	bool Success = (compress2((unsigned char*)OutBuffer, &DestSize, (unsigned char*)InBuffer, (unsigned long)InSize, Level) == Z_OK);
	*OutSize = DestSize;
	return Success;
}

size_t ZL_Compression::Decompress(const void* InBuf, size_t InSize, std::vector<unsigned char> &out, size_t Hint)
{
	z_stream z;
	memset(&z, 0, sizeof(z));
	if (sizeof(InSize) != sizeof(z.total_in) && !ZL_VERIFY(InSize <= 0xFFFFFFFF)) return 0; //zlib limit
	if (sizeof(Hint) != sizeof(z.avail_out) && !ZL_VERIFY(Hint <= 0xFFFFFFFF)) Hint = 0xFFFFFFFF; //zlib limit
	if (inflateInit(&z) != Z_OK) return 0;
	size_t head = out.size();
	if (Hint) { out.resize(head + (z.avail_out = (unsigned int)Hint)); z.next_out = &out[head]; }
	for (;;)
	{
		if (!z.avail_in && InSize) { z.next_in = (unsigned char*)InBuf + z.total_in; InSize -= (z.avail_in = (InSize < 1048576 ? (unsigned int)InSize : 1048576)); }
		if (!z.avail_out) { out.resize(out.size() + 1024); z.next_out = &out[head + z.total_out]; z.avail_out = (unsigned int)(out.size() - head - z.total_out); }
		int status = inflate(&z, Z_NO_FLUSH);
		if (status == Z_OK) continue;
		if (!ZL_VERIFY(status == Z_STREAM_END)) z.avail_out += z.total_out;
		break;
	}
	out.resize(out.size() - z.avail_out);
	ZL_ASSERT(inflateEnd(&z) == Z_OK);
	return out.size() - head;
}

bool ZL_Compression::Decompress(const void* InBuffer, size_t InSize, const void* OutBuffer, size_t* OutSize)
{
	if (sizeof(InSize) != sizeof(unsigned long) && !ZL_VERIFY(InSize <= 0xFFFFFFFF)) return false; //zlib limit
	if (sizeof(*OutSize) != sizeof(unsigned long) && !ZL_VERIFY(*OutSize <= 0xFFFFFFFF)) *OutSize = 0xFFFFFFFF; //zlib limit
	unsigned long DestSize = (unsigned long)*OutSize;
	bool Success = (uncompress((unsigned char*)OutBuffer, &DestSize, (unsigned char*)InBuffer, (unsigned long)InSize) == Z_OK);
	*OutSize = DestSize;
	return Success;
}

unsigned int ZL_Checksum::CRC32(const void* Data, size_t DataSize, unsigned int extend)
{
	return (unsigned int)crc32(extend, (unsigned char*)Data, (unsigned int)DataSize);
	//if (!Data) return 0;
	//unsigned int crcu32 = ~(unsigned int)extend;
	//unsigned char b, *p = (unsigned char*)Data;
	//// Karl Malbrain's compact CRC-32. See "A compact CCITT crc16 and crc32 C implementation that balances processor cache usage against speed": http://www.geocities.com/malbrain/
	//static const unsigned int s_crc32[16] = { 0, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c };
	//while (DataSize--) { b = *p++; crcu32 = (crcu32 >> 4) ^ s_crc32[(crcu32 & 0xF) ^ (b & 0xF)]; crcu32 = (crcu32 >> 4) ^ s_crc32[(crcu32 & 0xF) ^ (b >> 4)]; }
	//return ~crcu32;
}

unsigned int ZL_Checksum::Fast(const void* Data, size_t DataSize)
{
	unsigned int res = 0, *p = (unsigned int*)Data, *pMax = (unsigned int*)((char*)p + DataSize - 3);
	while (p < pMax) res = res*65599 + *(p++);
	switch (DataSize & 4) { case 0: return res; case 1: return res*65599 + *(char*)p; case 2: return res*65599 + *(short*)p; default: return res*65599 + ((((char*)p)[0]<<16)|(((char*)p)[1]<<8)|((char*)p)[2]); }
}

unsigned int ZL_Checksum::Fast4(const void* Data, size_t DataSize)
{
	unsigned int res = 0, *p = (unsigned int*)Data, *pMax = (unsigned int*)((char*)p + DataSize);
	while (p != pMax) res = res*65599 + *(p++);
	return res;
}

void ZL_Checksum::SHA1(const void* Data, size_t DataSize, unsigned char OutResult[20])
{
	struct SHA1_CTX
	{
		static void SHA1Transform(unsigned int* state, const void* buffer)
		{
			// Hash a single 512-bit block. This is the core of the algorithm
			unsigned int block[16];
			memcpy(block, buffer, 64);
			unsigned int a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
			// BLK0() and BLK() perform the initial expand
			// (R0+R1), R2, R3, R4 are the different operations used in SHA1
			#define SHA1ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))
			#define SHA1BLK0(i) (block[i] = (SHA1ROL(block[i],24)&0xFF00FF00)|(SHA1ROL(block[i],8)&0x00FF00FF))
			#define SHA1BLK(i) (block[i&15] = SHA1ROL(block[(i+13)&15]^block[(i+8)&15]^block[(i+2)&15]^block[i&15],1))
			#define SHA1R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+SHA1BLK0(i)+0x5A827999+SHA1ROL(v,5);w=SHA1ROL(w,30);
			#define SHA1R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+SHA1BLK(i)+0x5A827999+SHA1ROL(v,5);w=SHA1ROL(w,30);
			#define SHA1R2(v,w,x,y,z,i) z+=(w^x^y)+SHA1BLK(i)+0x6ED9EBA1+SHA1ROL(v,5);w=SHA1ROL(w,30);
			#define SHA1R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+SHA1BLK(i)+0x8F1BBCDC+SHA1ROL(v,5);w=SHA1ROL(w,30);
			#define SHA1R4(v,w,x,y,z,i) z+=(w^x^y)+SHA1BLK(i)+0xCA62C1D6+SHA1ROL(v,5);w=SHA1ROL(w,30);
			// 4 rounds of 20 operations each. Loop unrolled.
			SHA1R0(a,b,c,d,e, 0); SHA1R0(e,a,b,c,d, 1); SHA1R0(d,e,a,b,c, 2); SHA1R0(c,d,e,a,b, 3);
			SHA1R0(b,c,d,e,a, 4); SHA1R0(a,b,c,d,e, 5); SHA1R0(e,a,b,c,d, 6); SHA1R0(d,e,a,b,c, 7);
			SHA1R0(c,d,e,a,b, 8); SHA1R0(b,c,d,e,a, 9); SHA1R0(a,b,c,d,e,10); SHA1R0(e,a,b,c,d,11);
			SHA1R0(d,e,a,b,c,12); SHA1R0(c,d,e,a,b,13); SHA1R0(b,c,d,e,a,14); SHA1R0(a,b,c,d,e,15);
			SHA1R1(e,a,b,c,d,16); SHA1R1(d,e,a,b,c,17); SHA1R1(c,d,e,a,b,18); SHA1R1(b,c,d,e,a,19);
			SHA1R2(a,b,c,d,e,20); SHA1R2(e,a,b,c,d,21); SHA1R2(d,e,a,b,c,22); SHA1R2(c,d,e,a,b,23);
			SHA1R2(b,c,d,e,a,24); SHA1R2(a,b,c,d,e,25); SHA1R2(e,a,b,c,d,26); SHA1R2(d,e,a,b,c,27);
			SHA1R2(c,d,e,a,b,28); SHA1R2(b,c,d,e,a,29); SHA1R2(a,b,c,d,e,30); SHA1R2(e,a,b,c,d,31);
			SHA1R2(d,e,a,b,c,32); SHA1R2(c,d,e,a,b,33); SHA1R2(b,c,d,e,a,34); SHA1R2(a,b,c,d,e,35);
			SHA1R2(e,a,b,c,d,36); SHA1R2(d,e,a,b,c,37); SHA1R2(c,d,e,a,b,38); SHA1R2(b,c,d,e,a,39);
			SHA1R3(a,b,c,d,e,40); SHA1R3(e,a,b,c,d,41); SHA1R3(d,e,a,b,c,42); SHA1R3(c,d,e,a,b,43);
			SHA1R3(b,c,d,e,a,44); SHA1R3(a,b,c,d,e,45); SHA1R3(e,a,b,c,d,46); SHA1R3(d,e,a,b,c,47);
			SHA1R3(c,d,e,a,b,48); SHA1R3(b,c,d,e,a,49); SHA1R3(a,b,c,d,e,50); SHA1R3(e,a,b,c,d,51);
			SHA1R3(d,e,a,b,c,52); SHA1R3(c,d,e,a,b,53); SHA1R3(b,c,d,e,a,54); SHA1R3(a,b,c,d,e,55);
			SHA1R3(e,a,b,c,d,56); SHA1R3(d,e,a,b,c,57); SHA1R3(c,d,e,a,b,58); SHA1R3(b,c,d,e,a,59);
			SHA1R4(a,b,c,d,e,60); SHA1R4(e,a,b,c,d,61); SHA1R4(d,e,a,b,c,62); SHA1R4(c,d,e,a,b,63);
			SHA1R4(b,c,d,e,a,64); SHA1R4(a,b,c,d,e,65); SHA1R4(e,a,b,c,d,66); SHA1R4(d,e,a,b,c,67);
			SHA1R4(c,d,e,a,b,68); SHA1R4(b,c,d,e,a,69); SHA1R4(a,b,c,d,e,70); SHA1R4(e,a,b,c,d,71);
			SHA1R4(d,e,a,b,c,72); SHA1R4(c,d,e,a,b,73); SHA1R4(b,c,d,e,a,74); SHA1R4(a,b,c,d,e,75);
			SHA1R4(e,a,b,c,d,76); SHA1R4(d,e,a,b,c,77); SHA1R4(c,d,e,a,b,78); SHA1R4(b,c,d,e,a,79);
			// Add the working vars back into context.state[]
			state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
		}

		static void SHA1Process(SHA1_CTX* context, const unsigned char* data, size_t len)
		{
			size_t i; unsigned int j = context->count[0];
			if ((context->count[0] += (unsigned int)(len << 3)) < j) context->count[1]++;
			context->count[1] += (unsigned int)(len>>29);
			j = (j >> 3) & 63;
			if ((j + len) > 63)
			{
				memcpy(&context->buffer[j], data, (i = 64-j));
				SHA1Transform(context->state, context->buffer);
				for (; i + 63 < len; i += 64) SHA1Transform(context->state, &data[i]);
				j = 0;
			}
			else i = 0;
			memcpy(&context->buffer[j], &data[i], len - i);
		}

		unsigned int count[2];
		unsigned int state[5];
		unsigned char buffer[64];
	};

	// Initialize new context with initialization constants
	SHA1_CTX ctx;
	ctx.count[0] = ctx.count[1] = 0;
	ctx.state[0] = 0x67452301;
	ctx.state[1] = 0xEFCDAB89;
	ctx.state[2] = 0x98BADCFE;
	ctx.state[3] = 0x10325476;
	ctx.state[4] = 0xC3D2E1F0;

	SHA1_CTX::SHA1Process(&ctx, (const unsigned char*)Data, DataSize);

	// Add padding and return the message digest
	unsigned char finalcount[8];
	for (unsigned i = 0; i < 8; i++)  finalcount[i] = (unsigned char)((ctx.count[(i >= 4 ? 0 : 1)] >> ((3-(i & 3)) * 8) ) & 255);
	unsigned char c = 0200;
	SHA1_CTX::SHA1Process(&ctx, &c, 1);
	while ((ctx.count[0] & 504) != 448) { c = 0000; SHA1_CTX::SHA1Process(&ctx, &c, 1); }
	SHA1_CTX::SHA1Process(&ctx, finalcount, 8);
	for (unsigned j = 0; j < 20; j++) OutResult[j] = (unsigned char)((ctx.state[j>>2] >> ((3-(j & 3)) * 8) ) & 255);
}
