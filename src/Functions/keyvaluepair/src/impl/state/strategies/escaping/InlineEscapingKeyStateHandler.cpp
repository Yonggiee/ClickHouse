#include "InlineEscapingKeyStateHandler.h"
#include <Functions/keyvaluepair/src/impl/state/util/CharacterFinder.h>
#include <Functions/keyvaluepair/src/impl/state/util/EscapedCharacterReader.h>

namespace DB
{

static auto buildWaitRuntimeConfig(const ExtractorConfiguration & configuration)
{
    const auto & [key_value_delimiter, pair_delimiters, quoting_characters]
        = configuration;

    std::vector<char> special_characters;
    special_characters.push_back(EscapedCharacterReader::ESCAPE_CHARACTER);
    special_characters.push_back(key_value_delimiter);

    std::copy(pair_delimiters.begin(), pair_delimiters.end(), std::back_inserter(special_characters));

    return special_characters;
}

static auto buildReadRuntimeConfig(const ExtractorConfiguration & configuration)
{
    const auto & [key_value_delimiter, pair_delimiters, quoting_characters]
        = configuration;

    std::vector<char> needles;

    needles.push_back(EscapedCharacterReader::ESCAPE_CHARACTER);
    needles.push_back(key_value_delimiter);

    std::copy(quoting_characters.begin(), quoting_characters.end(), std::back_inserter(needles));
    std::copy(pair_delimiters.begin(), pair_delimiters.end(), std::back_inserter(needles));

    return needles;
}

static auto buildReadEnclosedRuntimeConfig(const ExtractorConfiguration & configuration)
{
    const auto & quoting_characters = configuration.quoting_characters;

    std::vector<char> needles;

    needles.push_back(EscapedCharacterReader::ESCAPE_CHARACTER);

    std::copy(quoting_characters.begin(), quoting_characters.end(), std::back_inserter(needles));

    return needles;
}

InlineEscapingKeyStateHandler::InlineEscapingKeyStateHandler(ExtractorConfiguration configuration_)
    : extractor_configuration(std::move(configuration_))
{
    auto wait_config = buildWaitRuntimeConfig(extractor_configuration);
    auto read_config = buildReadRuntimeConfig(extractor_configuration);
    auto read_enclosed_config = buildReadEnclosedRuntimeConfig(extractor_configuration);
    runtime_configuration = RuntimeConfiguration(wait_config, read_config, read_enclosed_config);
}

NextState InlineEscapingKeyStateHandler::wait(std::string_view file, size_t pos) const
{
    BoundsSafeCharacterFinder finder;

    const auto & quoting_characters = extractor_configuration.quoting_characters;

    while (auto character_position_opt = finder.find_first_not(file, pos, runtime_configuration.wait_configuration))
    {
        auto character_position = *character_position_opt;
        auto character = file[character_position];

        if (quoting_characters.contains(character))
        {
            return {character_position + 1u, State::READING_ENCLOSED_KEY};
        }
        else
        {
            return {character_position, State::READING_KEY};
        }
    }

    return {file.size(), State::END};
}

/*
 * I only need to iteratively copy stuff if there are escape sequences. If not, views are sufficient.
 * TSKV has a nice catch for that, implementers kept an auxiliary string to hold copied characters.
 * If I find a key value delimiter and that is empty, I do not need to copy? hm,m hm hm
 * */

NextState InlineEscapingKeyStateHandler::read(std::string_view file, size_t pos, ElementType & key) const
{
    BoundsSafeCharacterFinder finder;

    const auto & [key_value_delimiter, pair_delimiters, quoting_characters]
        = extractor_configuration;

    key.clear();

    /*
     * Maybe modify finder return type to be the actual pos. In case of failures, it shall return pointer to the end.
     * It might help updating current pos?
     * */

    while (auto character_position_opt = finder.find_first(file, pos, runtime_configuration.read_configuration))
    {
        auto character_position = *character_position_opt;
        auto character = file[character_position];
        auto next_pos = character_position + 1u;

        if (EscapedCharacterReader::isEscapeCharacter(character))
        {
            for (auto i = pos; i < character_position; i++)
            {
                key.push_back(file[i]);
            }

            auto [next_byte_ptr, escaped_characters] = EscapedCharacterReader::read(file, character_position);
            next_pos = next_byte_ptr - file.begin();

            if (escaped_characters.empty())
            {
                return {next_pos, State::WAITING_KEY};
            }
            else
            {
                for (auto escaped_character : escaped_characters)
                {
                    key.push_back(escaped_character);
                }
            }
        }
        else if (character == key_value_delimiter)
        {
            // todo try to optimize with resize and memcpy
            for (auto i = pos; i < character_position; i++)
            {
                key.push_back(file[i]);
            }

            return {next_pos, State::WAITING_VALUE};
        }
        // pair delimiter
        else if (pair_delimiters.contains(character))
        {
            return {next_pos, State::WAITING_KEY};
        }

        pos = next_pos;
    }

    // might be problematic in case string reaches the end and I haven't copied anything over to key

    return {file.size(), State::END};
}

NextState InlineEscapingKeyStateHandler::readEnclosed(std::string_view file, size_t pos, ElementType & key) const
{
    BoundsSafeCharacterFinder finder;

    const auto & quoting_characters = extractor_configuration.quoting_characters;

    key.clear();

    while (auto character_position_opt = finder.find_first(file, pos, runtime_configuration.read_enclosed_configuration))
    {
        auto character_position = *character_position_opt;
        auto character = file[character_position];
        auto next_pos = character_position + 1u;

        if (character == EscapedCharacterReader::ESCAPE_CHARACTER)
        {
            for (auto i = pos; i < character_position; i++)
            {
                key.push_back(file[i]);
            }

            auto [next_byte_ptr, escaped_characters] = EscapedCharacterReader::read(file, character_position);
            next_pos = next_byte_ptr - file.begin();

            if (escaped_characters.empty())
            {
                return {next_pos, State::WAITING_KEY};
            }
            else
            {
                for (auto escaped_character : escaped_characters)
                {
                    key.push_back(escaped_character);
                }
            }
        }
        else if(quoting_characters.contains(character))
        {
            // todo try to optimize with resize and memcpy
            for (auto i = pos; i < character_position; i++)
            {
                key.push_back(file[i]);
            }

            if (key.empty())
            {
                return {next_pos, State::WAITING_KEY};
            }

            return {next_pos, State::READING_KV_DELIMITER};
        }

        pos = next_pos;
    }

    return {file.size(), State::END};
}

NextState InlineEscapingKeyStateHandler::readKeyValueDelimiter(std::string_view file, size_t pos) const
{
    if (pos == file.size())
    {
        return {pos, State::END};
    }
    else
    {
        const auto current_character = file[pos++];
        return {pos, extractor_configuration.key_value_delimiter == current_character ? State::WAITING_VALUE : State::WAITING_KEY};
    }
}

}
