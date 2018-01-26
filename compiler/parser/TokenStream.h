#pragma once

#include <istream>
#include <regex>

#include "Token.h"

namespace MaximParser {

    class TokenStream {
    public:

        explicit TokenStream(const std::string &data);

        void restart();

        Token next();

        const Token &peek();

    private:
        std::string data;
        std::string::iterator dataBegin;

        bool hasBuffer = false;
        Token peekBuffer;

        Token processNext();

        static constexpr std::size_t matchCount = 35;

        using PairType = std::pair<std::regex, Token::Type>;
        using PairListType = std::array<PairType, matchCount>;

        static PairListType matches;
        static PairType getToken(const std::string &regex, Token::Type type);
    };

}