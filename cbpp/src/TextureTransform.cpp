
#include "ComputeProcessor.h"

#include <map>
#include <cassert>


namespace
{
	struct Keyword
	{
		Keyword(const char* text)
			: text(text)
			, length(strlen(text))
			, hash(cmpHash(text, length))
		{
		}

		const char* text;

		cmpU32 length;

		cmpU32 hash;
	};

	// Texture types
	Keyword KEYWORD_Texture3Du("Texture3Du");
	Keyword KEYWORD_Texture3Dn("Texture3Dn");
	Keyword KEYWORD_Texture2Du("Texture2Du");
	Keyword KEYWORD_Texture2Dn("Texture2Dn");
	Keyword KEYWORD_Texture1Du("Texture1Du");
	Keyword KEYWORD_Texture1Dn("Texture1Dn");

	// Texture texel types
	Keyword KEYWORD_char("char");
	Keyword KEYWORD_short("short");
	Keyword KEYWORD_int("int");
	Keyword KEYWORD_long("long");
	Keyword KEYWORD_float("float");
	Keyword KEYWORD_signed("signed");
	Keyword KEYWORD_unsigned("unsigned");

	Keyword KEYWORD_cmp_texture_type("cmp_texture_type");

	// Texture dimensions
	Keyword KEYWORD_1("1");
	Keyword KEYWORD_2("2");
	Keyword KEYWORD_3("3");

	// CUDA read types
	Keyword KEYWORD_cudaReadModeElementType("cudaReadModeElementType");
	Keyword KEYWORD_cudaReadModeNormalizedFloat("cudaReadModeNormalizedFloat");


	cmpU32 CombineHash(cmpU32 combined_hash, cmpU32 hash)
	{
		// A sequence of 32 uniformly random bits so that each bit of the combined hash is changed on application
		// Derived from the golden ratio: UINT_MAX / ((1 + sqrt(5)) / 2)
		// In reality it's just an arbitrary value which happens to work well, avoiding mapping all zeros to zeros.
		// http://burtleburtle.net/bob/hash/doobs.html
		static cmpU32 random_bits = 0x9E3779B9;
		combined_hash ^= hash + random_bits + (combined_hash << 6) + (combined_hash >> 2);
		return combined_hash;
	}
}


//
// Reference to a texture type in the source file.
// Example: "Texture3Dn<short>"
//
struct TextureRef
{
	TextureRef()
		: node(0)
		, line(0)
		, keyword_token(0)
		, type_token(0)
		, nb_type_tokens(0)
		, end_of_type_token(0)
		, name_token(0)
	{
	}

	// Pointer to the statement, typedef or function parameter list
	cmpNode* node;

	// Line the texture reference was found on
	cmpU32 line;

	// Texture keyword
	cmpToken* keyword_token;

	// Texel type keyword that may consist of two tokens, e.g. "unsigned int"
	cmpToken* type_token;
	cmpU32 nb_type_tokens;

	// Points to a token one place beyond the last token that defines the type
	cmpToken* end_of_type_token;

	// Only set for function parameters
	cmpToken* name_token;
};


//
// A vector of all texture ref instances that share the same dimension/type/read mode
//
typedef std::vector<TextureRef> TextureRefs;


//
// A map from the hash a texture reference to all its found instances
//
typedef std::map<cmpU32, TextureRefs> TextureRefsMap;


//
// Searches the AST of a source file for all texture type references
//
class FindTextureRefs : public INodeVisitor
{
public:
	FindTextureRefs()
		: m_LastError(cmpError_CreateOK())
	{
		m_TextureMatches = MatchHashes(
			KEYWORD_Texture3Dn.hash,
			KEYWORD_Texture3Du.hash,
			KEYWORD_Texture2Dn.hash,
			KEYWORD_Texture2Du.hash,
			KEYWORD_Texture1Dn.hash,
			KEYWORD_Texture1Du.hash);

		m_TypeMatches = MatchHashes(
			KEYWORD_char.hash,
			KEYWORD_short.hash,
			KEYWORD_int.hash,
			KEYWORD_long.hash,
			KEYWORD_float.hash,
			KEYWORD_signed.hash,
			KEYWORD_unsigned.hash);
	}


	bool Visit(const ComputeProcessor& processor, cmpNode& node)
	{
		// Filter out the node types we're not interested in
		if (node.type != cmpNode_Statement &&
			node.type != cmpNode_FunctionParams &&
			node.type != cmpNode_Typedef)
			return true;

		// Search for any of the texture keywords
		TokenIterator iterator(node);
		if (iterator.SeekToken(m_TextureMatches) == 0)
			return true;

		const char* filename = processor.Filename().c_str();

		// Start the texture reference off with its node/token and token hash
		TextureRef ref;
		ref.node = &node;
		ref.line = iterator.token->line;
		ref.keyword_token = iterator.token;
		cmpU32 combined_hash = iterator.token->hash;
		++iterator;

		// Ensure '<' follows
		if (iterator.ExpectToken(MatchTypes(cmpToken_LAngle)) == 0)
		{
			m_LastError = cmpError_Create("%s(%d): Expecting '<'", filename, iterator.token->line);
			return false;
		}
		++iterator;

		// Ensure a type name is next
		cmpToken* type_token_0 = iterator.ExpectToken(m_TypeMatches);
		if (type_token_0 == 0)
		{
			m_LastError = cmpError_Create("%s(%d): Expecting a type name", filename, iterator.token->line);
			return false;
		}
		ref.type_token = type_token_0;
		ref.nb_type_tokens = 1;
		combined_hash = CombineHash(combined_hash, type_token_0->hash);
		++iterator;

		// If the type name was signed/unsigned, expect the rest of the type name
		if (type_token_0->hash == KEYWORD_signed.hash || type_token_0->hash == KEYWORD_unsigned.hash)
		{
			const cmpToken* type_token_1 = iterator.ExpectToken(m_TypeMatches);
			if (type_token_1 == 0)
			{
				m_LastError = cmpError_Create("%s(%d): Expecting a type name after unsigned/signed", filename, iterator.token->line);
				return false;
			}
			if (type_token_1->hash == KEYWORD_signed.hash || type_token_1->hash == KEYWORD_unsigned.hash)
			{
				m_LastError = cmpError_Create("%s(%d): Not expecting unsigned/signed twice", filename, iterator.token->line);
				return false;
			}

			ref.nb_type_tokens = 2;
			combined_hash = CombineHash(combined_hash, type_token_1->hash);
			++iterator;
		}

		// Ensure '>' closes the type naming
		if (iterator.ExpectToken(MatchTypes(cmpToken_RAngle)) == 0)
		{
			m_LastError = cmpError_Create("%s(%d): Expecting '>'", filename, iterator.token->line);
			return false;
		}
		ref.end_of_type_token = iterator.token;
		++iterator;

		// Ensure that function parameters have a name
		if (node.type == cmpNode_FunctionParams)
		{
			if (iterator.ExpectToken(MatchTypes(cmpToken_Symbol)) == 0)
			{
				m_LastError = cmpError_Create("%s(%d): Expecting function parameter to have a name", filename, iterator.token->line);
				return false;
			}
			ref.name_token = iterator.token;
			++iterator;
		}

		// Record the texture reference
		m_TextureRefsMap[combined_hash].push_back(ref);
		return true;
	}


	const TextureRefsMap& Results() const
	{
		return m_TextureRefsMap;
	}


	const cmpError& LastError() const
	{
		return m_LastError;
	}


private:
	MatchHashes m_TextureMatches;
	MatchHashes m_TypeMatches;

	TextureRefsMap m_TextureRefsMap;

	cmpError m_LastError;
};


namespace
{
	const TextureRef& FindFirstTextureRef(const TextureRefs& refs)
	{
		const TextureRef* found_ref = 0;
		cmpU32 first_line = UINT_MAX;

		// Linear search through all texture refs looking for the one that occurs first
		for (size_t i = 0; i < refs.size(); i++)
		{
			const TextureRef& ref = refs[i];
			cmpU32 line = ref.line;
			if (line < first_line)
			{
				found_ref = &ref;
				first_line = line;
			}
		}

		assert(found_ref != 0);
		return *found_ref;
	}
}


//
// Persistent pointers to text that cmpToken objects can reference. TextureType objects may be moved
// around in memory, ruling out embedded char arrays. std::string may make small-string optimisations
// by embedding local char arrays so that's out of the window, too.
// Transfers ownership on copy.
//
struct String
{
	String()
		: text(0)
		, length(0)
	{
	}


	String(const std::string& source)
		: text(0)
		, length(0)
	{
		// Copy and null terminate
		length = source.length();
		text = new char[length + 1];
		memcpy(this->text, source.c_str(), length);
		this->text[length] = 0;
	}


	String(const String& rhs)
		: text(rhs.text)
		, length(rhs.length)
	{
		// Take ownership
		rhs.text = 0;
	}


	~String()
	{
		if (text != 0)
			delete [] text;
	}


	String& operator = (const String& rhs)
	{
		assert(text == 0);

		// Take ownership
		text = rhs.text;
		length = rhs.length;
		rhs.text = 0;
		return *this;
	}

	mutable char* text;
	size_t length;
};


class TextureType
{
public:
	TextureType(cmpU32 texture_refs_key)
		: m_TextureRefsKey(texture_refs_key)
		, m_FirstToken(0)
		, m_LastToken(0)
		, m_LastError(cmpError_CreateOK())
	{
	}


	~TextureType()
	{
		// Responsibility for cleaning created tokens belongs with this object
		while (m_FirstToken != 0)
		{
			cmpToken* next = m_FirstToken->next;
			cmpToken_Destroy(m_FirstToken);
			m_FirstToken = next;
		}
	}


	bool AddTypeDeclaration(const TextureRef& ref, int unique_index)
	{
		// Start off the macro call
		if (!AddToken(KEYWORD_cmp_texture_type, ref.line))
			return false;
		if (!AddToken(cmpToken_LBracket, "(", 1, ref.line))
			return false;

		// Add the texel type name tokens
		const cmpToken* type_token = ref.type_token;
		assert(type_token != 0);
		if (!AddToken(cmpToken_Symbol, type_token->start, type_token->length, ref.line))
			return false;
		if (ref.nb_type_tokens > 1)
		{
			type_token = type_token->next;
			assert(type_token != 0);
			if (!AddToken(cmpToken_Symbol, type_token->start, type_token->length, ref.line))
				return false;
		}
		if (!AddToken(cmpToken_Comma, ",", 1, ref.line))
			return false;

		// Decode number of texture dimensions and read type
		const cmpToken* keyword_token = ref.keyword_token;
		assert(keyword_token != 0);
		assert(keyword_token->length == 10);
		const char* keyword = keyword_token->start;
		cmpU32 dimensions = keyword[7] - '0';
		assert(dimensions < 4);
		char read_type = keyword[9];
		assert(read_type == 'u' || read_type == 'n');

		// Add the texture dimension token
		const Keyword* kw_dimensions = 0;
		switch (dimensions)
		{
			case 1: kw_dimensions = &KEYWORD_1; break;
			case 2: kw_dimensions = &KEYWORD_2; break;
			case 3: kw_dimensions = &KEYWORD_3; break;
		}
		assert(kw_dimensions != 0);
		if (!AddToken(cmpToken_Number, kw_dimensions->text, kw_dimensions->length, ref.line))
			return false;
		if (!AddToken(cmpToken_Comma, ",", 1, ref.line))
			return false;

		// Add read type token
		if (read_type == 'u')
		{
			if (!AddToken(KEYWORD_cudaReadModeElementType, ref.line))
				return false;
		}
		else
		{
			if (!AddToken(KEYWORD_cudaReadModeNormalizedFloat, ref.line))
				return false;
		}
		if (!AddToken(cmpToken_Comma, ",", 1, ref.line))
			return false;

		// Generate a unique type name and add as a symbol token
		char type_name[64];
		sprintf(type_name, "__TextureTypeName_%d__", unique_index);
		m_Name = String(type_name);
		if (!AddToken(cmpToken_Symbol, m_Name.text, m_Name.length, ref.line))
			return false;

		// Close the statement
		if (!AddToken(cmpToken_RBracket, ")", 1, ref.line))
			return false;
		if (!AddToken(cmpToken_SemiColon, ";", 1, ref.line))
			return false;

		// Create the containing node (to be deleted by the parse tree)
		cmpNode* type_node;
		m_LastError = cmpNode_CreateEmpty(&type_node);
		if (!cmpError_OK(&m_LastError))
			return false;
		type_node->type = cmpNode_UserTokens;
		type_node->first_token = m_FirstToken;
		type_node->last_token = m_LastToken;

		// Always insert right before typedefs
		cmpNode* insert_before_node = ref.node;
		if (insert_before_node->type != cmpNode_Typedef)
		{
			// Anything else must be placed just before the parent function definition/declaration
			while (insert_before_node != 0 &&
				   insert_before_node->type != cmpNode_FunctionDefn &&
				   insert_before_node->type != cmpNode_FunctionDecl)
				insert_before_node = insert_before_node->parent;
		}
		if (insert_before_node == NULL)
		{
			m_LastError = cmpError_Create("Failed to find good location for type declaration");
			return false;
		}
		cmpNode_AddBefore(insert_before_node, type_node);

		return true;
	}


	bool ReplaceTypeInstance(const TextureRef& ref)
	{
		// Create the single replacement token
		cmpToken* token;
		m_LastError = cmpToken_Create(&token, cmpToken_Symbol, m_Name.text, m_Name.length, ref.line);
		if (!cmpError_OK(&m_LastError))
			return false;

		// Cut out the original tokens and replace with the new one
		cmpToken* first_token = ref.keyword_token;
		cmpToken* last_token = ref.end_of_type_token;
		token->prev = first_token->prev;
		token->prev->next = token;
		token->next = last_token->next;
		token->next->prev = token;

		// DELETE


		return true;
	}


	cmpU32 TextureRefsKey() const
	{
		return m_TextureRefsKey;
	}


	cmpError LastError() const
	{
		return m_LastError;
	}


private:
	cmpToken* AddToken(enum cmpTokenType type, const char* start, cmpU32 length, cmpU32 line)
	{
		cmpToken* token;
		m_LastError = cmpToken_Create(&token, type, start, length, line);
		if (!cmpError_OK(&m_LastError))
			return 0;
		cmpToken_AddToList(&m_FirstToken, &m_LastToken, token);
		return token;
	}


	cmpToken* AddToken(const Keyword& keyword, cmpU32 line)
	{
		// Create a symbol token using globally persistent keyword text
		cmpToken* token = AddToken(cmpToken_Symbol, keyword.text, keyword.length, line);
		if (token != 0)
			token->hash = keyword.hash;
		return token;
	}


	// Key used to lookup texture refs that use this type
	cmpU32 m_TextureRefsKey;

	// Name of the uniquely generated string type
	String m_Name;

	// Linked list of tokens created by this texture type
	cmpToken* m_FirstToken;
	cmpToken* m_LastToken;

	cmpError m_LastError;
};


class TextureTransform : public ITransform
{
public:
	TextureTransform()
		: m_UniqueTypeIndex(0)
	{
	}

private:
	cmpError Apply(ComputeProcessor& processor)
	{
		// Find all texture references
		FindTextureRefs ftr;
		if (!processor.VisitNodes(&ftr))
			return ftr.LastError();
		const TextureRefsMap& refs_map = ftr.Results();
		if (refs_map.size() == 0)
			return cmpError_CreateOK();

		// Build a list of all unique texture types introduced
		for (TextureRefsMap::const_iterator i = refs_map.begin(); i != refs_map.end(); ++i)
		{
			const TextureRefs& refs = i->second;

			// Generate a texture type from the first instance of this texture reference
			const TextureRef& first_ref = FindFirstTextureRef(refs);
			TextureType texture_type(i->first);

			// Place a type declaration somewhere before the first node
			if (!texture_type.AddTypeDeclaration(first_ref, m_UniqueTypeIndex++))
				return texture_type.LastError();

			m_TextureTypes.push_back(texture_type);
		}

		// Replace the type of all texture references with the newly generated unique ones
		for (size_t i = 0; i < m_TextureTypes.size(); i++)
		{
			TextureType& texture_type = m_TextureTypes[i];
			const TextureRefs& refs = refs_map.find(texture_type.TextureRefsKey())->second;

			for (size_t j = 0; j < refs.size(); j++)
			{
				if (!texture_type.ReplaceTypeInstance(refs[j]))
					return texture_type.LastError();
			}
		}

		return cmpError_CreateOK();
	}

	// Used to generate unique type names for texture references
	int m_UniqueTypeIndex;

	std::vector<TextureType> m_TextureTypes;
};


// Register transform
static TransformDesc<TextureTransform> g_TextureTransform;