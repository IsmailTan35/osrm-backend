#ifndef OSMIUM_IO_DETAIL_OPL_PARSER_FUNCTIONS_HPP
#define OSMIUM_IO_DETAIL_OPL_PARSER_FUNCTIONS_HPP

/*

This file is part of Osmium (https://osmcode.org/libosmium).

Copyright 2013-2020 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/io/detail/string_util.hpp>
#include <osmium/io/error.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/changeset.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/osm/way.hpp>

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace osmium {

    namespace builder {
        class Builder;
    } // namespace builder

    /**
     * Exception thrown when there was a problem with parsing the OPL format
     * of a file.
     */
    struct opl_error : public io_error {

        uint64_t line = 0;
        uint64_t column = 0;
        const char* data;
        std::string msg;

        explicit opl_error(const std::string& what, const char* d = nullptr) :
            io_error(std::string("OPL error: ") + what),
            data(d),
            msg("OPL error: ") {
            msg.append(what);
        }

        explicit opl_error(const char* what, const char* d = nullptr) :
            io_error(std::string{"OPL error: "} + what),
            data(d),
            msg("OPL error: ") {
            msg.append(what);
        }

        void set_pos(uint64_t l, uint64_t col) {
            line = l;
            column = col;
            msg.append(" on line ");
            msg.append(std::to_string(line));
            msg.append(" column ");
            msg.append(std::to_string(column));
        }

        const char* what() const noexcept override {
            return msg.c_str();
        }

    }; // struct opl_error

    namespace io {

        namespace detail {

            /**
             * Consume consecutive space and tab characters. There must be
             * at least one.
             */
            inline void opl_parse_space(const char** s) {
                if (**s != ' ' && **s != '\t') {
                    throw opl_error{"expected space or tab character", *s};
                }
                do {
                    ++*s;
                } while (**s == ' ' || **s == '\t');
            }

            /**
             * Check whether s points to something else than the end of the
             * string or a space or tab.
             */
            inline bool opl_non_empty(const char *s) {
                return *s != '\0' && *s != ' ' && *s != '\t';
            }

            /**
             * Skip to the next space or tab character or the end of the
             * string.
             */
            inline const char* opl_skip_section(const char** s) noexcept {
                while (opl_non_empty(*s)) {
                    ++*s;
                }
                return *s;
            }

            /**
             * Parse OPL-escaped strings with hex code with a '%' at the end.
             * Appends resulting unicode character to the result string.
             *
             * Returns a pointer to next character that needs to be consumed.
             */
            inline void opl_parse_escaped(const char** data, std::string& result) {
                const char* s = *data;
                uint32_t value = 0;
                const int max_length = sizeof(value) * 2 /* hex chars per byte */;
                int length = 0;
                while (++length <= max_length) {
                    if (*s == '\0') {
                        throw opl_error{"eol", s};
                    }
                    if (*s == '%') {
                        ++s;
                        append_codepoint_as_utf8(value, std::back_inserter(result));
                        *data = s;
                        return;
                    }
                    value <<= 4U;
                    if (*s >= '0' && *s <= '9') {
                        value += *s - '0';
                    } else if (*s >= 'a' && *s <= 'f') {
                        value += *s - 'a' + 10;
                    } else if (*s >= 'A' && *s <= 'F') {
                        value += *s - 'A' + 10;
                    } else {
                        throw opl_error{"not a hex char", s};
                    }
                    ++s;
                }
                throw opl_error{"hex escape too long", s};
            }

            /**
             * Parse a string up to end of string or next space, tab, comma, or
             * equal sign.
             *
             * Appends characters to the result string.
             *
             * Returns a pointer to next character that needs to be consumed.
             */
            inline void opl_parse_string(const char** data, std::string& result) {
                const char* s = *data;
                while (true) {
                    if (*s == '\0' || *s == ' ' || *s == '\t' || *s == ',' || *s == '=') {
                        break;
                    }
                    if (*s == '%') {
                        ++s;
                        opl_parse_escaped(&s, result);
                    } else {
                        result += *s;
                        ++s;
                    }
                }
                *data = s;
            }

            // Arbitrary limit how long integers can get
            enum {
                max_int_len = 16
            };

            template <typename T>
            inline T opl_parse_int(const char** s) {
                if (**s == '\0') {
                    throw opl_error{"expected integer", *s};
                }
                const bool negative = (**s == '-');
                if (negative) {
                    ++*s;
                }

                int64_t value = 0;

                int n = max_int_len;
                while (**s >= '0' && **s <= '9') {
                    if (--n == 0) {
                        throw opl_error{"integer too long", *s};
                    }
                    value *= 10;
                    value += **s - '0';
                    ++*s;
                }

                if (n == max_int_len) {
                    throw opl_error{"expected integer", *s};
                }

                if (negative) {
                    value = -value;
                    if (value < std::numeric_limits<T>::min()) {
                        throw opl_error{"integer too long", *s};
                    }
                } else {
                    if (value > std::numeric_limits<T>::max()) {
                        throw opl_error{"integer too long", *s};
                    }
                }

                return T(value);
            }

            inline osmium::object_id_type opl_parse_id(const char** s) {
                return opl_parse_int<osmium::object_id_type>(s);
            }

            inline osmium::changeset_id_type opl_parse_changeset_id(const char** s) {
                return opl_parse_int<osmium::changeset_id_type>(s);
            }

            inline osmium::object_version_type opl_parse_version(const char** s) {
                return opl_parse_int<osmium::object_version_type>(s);
            }

            inline bool opl_parse_visible(const char** data) {
                if (**data == 'V') {
                    ++*data;
                    return true;
                }
                if (**data == 'D') {
                    ++*data;
                    return false;
                }
                throw opl_error{"invalid visible flag", *data};
            }

            inline osmium::user_id_type opl_parse_uid(const char** s) {
                return opl_parse_int<osmium::user_id_type>(s);
            }

            inline osmium::Timestamp opl_parse_timestamp(const char** s) {
                try {
                    if (**s == '\0' || **s == ' ' || **s == '\t') {
                        return osmium::Timestamp{};
                    }
                    osmium::Timestamp timestamp{*s};
                    *s += 20;
                    return timestamp;
                } catch (const std::invalid_argument&) {
                    throw opl_error{"can not parse timestamp", *s};
                }
            }

            /**
             * Check if data points to given character and consume it.
             * Throw error otherwise.
             */
            inline void opl_parse_char(const char** data, char c) {
                if (**data == c) {
                    ++*data;
                    return;
                }
                std::string msg{"expected '"};
                msg += c;
                msg += "'";
                throw opl_error{msg, *data};
            }

            /**
             * Parse a list of tags in the format 'key=value,key=value,...'
             *
             * Tags will be added to the buffer using a TagListBuilder.
             */
            inline void opl_parse_tags(const char* s, osmium::memory::Buffer& buffer, osmium::builder::Builder* parent_builder = nullptr) {
                osmium::builder::TagListBuilder builder{buffer, parent_builder};
                std::string key;
                std::string value;
                while (true) {
                    opl_parse_string(&s, key);
                    opl_parse_char(&s, '=');
                    opl_parse_string(&s, value);
                    builder.add_tag(key, value);
                    if (*s == ' ' || *s == '\t' || *s == '\0') {
                        break;
                    }
                    opl_parse_char(&s, ',');
                    key.clear();
                    value.clear();
                }
            }

            /**
             * Parse a number of nodes in the format "nID,nID,nID..."
             *
             * Nodes will be added to the buffer using a WayNodeListBuilder.
             */
            inline void opl_parse_way_nodes(const char* s, const char* e, osmium::memory::Buffer& buffer, osmium::builder::WayBuilder* parent_builder = nullptr) {
                if (s == e) {
                    return;
                }
                osmium::builder::WayNodeListBuilder builder{buffer, parent_builder};

                while (s < e) {
                    opl_parse_char(&s, 'n');
                    if (s == e) {
                        throw opl_error{"expected integer", s};
                    }

                    const osmium::object_id_type ref = opl_parse_id(&s);
                    if (s == e) {
                        builder.add_node_ref(ref);
                        return;
                    }

                    osmium::Location location;
                    if (*s == 'x') {
                        ++s;
                        location.set_lon_partial(&s);
                        if (*s == 'y') {
                            ++s;
                            location.set_lat_partial(&s);
                        }
                    }

                    builder.add_node_ref(ref, location);

                    if (s == e) {
                        return;
                    }

                    opl_parse_char(&s, ',');
                }
            }

            inline void opl_parse_node(const char** data, osmium::memory::Buffer& buffer) {
                osmium::builder::NodeBuilder builder{buffer};

                builder.set_id(opl_parse_id(data));

                const char* tags_begin = nullptr;

                bool has_version = false;
                bool has_visible = false;
                bool has_changeset_id = false;
                bool has_timestamp = false;
                bool has_uid = false;
                bool has_user = false;
                bool has_tags = false;
                bool has_lon = false;
                bool has_lat = false;

                std::string user;
                osmium::Location location;
                while (**data) {
                    opl_parse_space(data);
                    const char c = **data;
                    if (c == '\0') {
                        break;
                    }
                    ++(*data);
                    switch (c) {
                        case 'v':
                            if (has_version) {
                                throw opl_error{"Duplicate attribute: version (v)"};
                            }
                            has_version = true;
                            builder.set_version(opl_parse_version(data));
                            break;
                        case 'd':
                            if (has_visible) {
                                throw opl_error{"Duplicate attribute: visible (d)"};
                            }
                            has_visible = true;
                            builder.set_visible(opl_parse_visible(data));
                            break;
                        case 'c':
                            if (has_changeset_id) {
                                throw opl_error{"Duplicate attribute: changeset_id (c)"};
                            }
                            has_changeset_id = true;
                            builder.set_changeset(opl_parse_changeset_id(data));
                            break;
                        case 't':
                            if (has_timestamp) {
                                throw opl_error{"Duplicate attribute: timestamp (t)"};
                            }
                            has_timestamp = true;
                            builder.set_timestamp(opl_parse_timestamp(data));
                            break;
                        case 'i':
                            if (has_uid) {
                                throw opl_error{"Duplicate attribute: uid (i)"};
                            }
                            has_uid = true;
                            builder.set_uid(opl_parse_uid(data));
                            break;
                        case 'u':
                            if (has_user) {
                                throw opl_error{"Duplicate attribute: user (u)"};
                            }
                            has_user = true;
                            opl_parse_string(data, user);
                            break;
                        case 'T':
                            if (has_tags) {
                                throw opl_error{"Duplicate attribute: tags (T)"};
                            }
                            has_tags = true;
                            if (opl_non_empty(*data)) {
                                tags_begin = *data;
                                opl_skip_section(data);
                            }
                            break;
                        case 'x':
                            if (has_lon) {
                                throw opl_error{"Duplicate attribute: lon (x)"};
                            }
                            has_lon = true;
                            if (opl_non_empty(*data)) {
                                location.set_lon_partial(data);
                            }
                            break;
                        case 'y':
                            if (has_lat) {
                                throw opl_error{"Duplicate attribute: lat (y)"};
                            }
                            has_lat = true;
                            if (opl_non_empty(*data)) {
                                location.set_lat_partial(data);
                            }
                            break;
                        default:
                            --(*data);
                            throw opl_error{"unknown attribute", *data};
                    }
                }

                if (location.valid()) {
                    builder.set_location(location);
                }

                builder.set_user(user);

                if (tags_begin) {
                    opl_parse_tags(tags_begin, buffer, &builder);
                }
            }

            inline void opl_parse_way(const char** data, osmium::memory::Buffer& buffer) {
                osmium::builder::WayBuilder builder{buffer};

                builder.set_id(opl_parse_id(data));

                const char* tags_begin = nullptr;

                const char* nodes_begin = nullptr;
                const char* nodes_end = nullptr;

                bool has_version = false;
                bool has_visible = false;
                bool has_changeset_id = false;
                bool has_timestamp = false;
                bool has_uid = false;
                bool has_user = false;
                bool has_tags = false;
                bool has_nodes = false;

                std::string user;
                while (**data) {
                    opl_parse_space(data);
                    const char c = **data;
                    if (c == '\0') {
                        break;
                    }
                    ++(*data);
                    switch (c) {
                        case 'v':
                            if (has_version) {
                                throw opl_error{"Duplicate attribute: version (v)"};
                            }
                            has_version = true;
                            builder.set_version(opl_parse_version(data));
                            break;
                        case 'd':
                            if (has_visible) {
                                throw opl_error{"Duplicate attribute: visible (d)"};
                            }
                            has_visible = true;
                            builder.set_visible(opl_parse_visible(data));
                            break;
                        case 'c':
                            if (has_changeset_id) {
                                throw opl_error{"Duplicate attribute: changeset_id (c)"};
                            }
                            has_changeset_id = true;
                            builder.set_changeset(opl_parse_changeset_id(data));
                            break;
                        case 't':
                            if (has_timestamp) {
                                throw opl_error{"Duplicate attribute: timestamp (t)"};
                            }
                            has_timestamp = true;
                            builder.set_timestamp(opl_parse_timestamp(data));
                            break;
                        case 'i':
                            if (has_uid) {
                                throw opl_error{"Duplicate attribute: uid (i)"};
                            }
                            has_uid = true;
                            builder.set_uid(opl_parse_uid(data));
                            break;
                        case 'u':
                            if (has_user) {
                                throw opl_error{"Duplicate attribute: user (u)"};
                            }
                            has_user = true;
                            opl_parse_string(data, user);
                            break;
                        case 'T':
                            if (has_tags) {
                                throw opl_error{"Duplicate attribute: tags (T)"};
                            }
                            has_tags = true;
                            if (opl_non_empty(*data)) {
                                tags_begin = *data;
                                opl_skip_section(data);
                            }
                            break;
                        case 'N':
                            if (has_nodes) {
                                throw opl_error{"Duplicate attribute: nodes (N)"};
                            }
                            has_nodes = true;
                            nodes_begin = *data;
                            nodes_end = opl_skip_section(data);
                            break;
                        default:
                            --(*data);
                            throw opl_error{"unknown attribute", *data};
                    }
                }

                builder.set_user(user);

                if (tags_begin) {
                    opl_parse_tags(tags_begin, buffer, &builder);
                }

                opl_parse_way_nodes(nodes_begin, nodes_end, buffer, &builder);
            }

            inline void opl_parse_relation_members(const char* s, const char* e, osmium::memory::Buffer& buffer, osmium::builder::RelationBuilder* parent_builder = nullptr) {
                if (s == e) {
                    return;
                }
                osmium::builder::RelationMemberListBuilder builder{buffer, parent_builder};

                while (s < e) {
                    osmium::item_type type = osmium::char_to_item_type(*s);
                    if (type != osmium::item_type::node &&
                        type != osmium::item_type::way &&
                        type != osmium::item_type::relation) {
                        throw opl_error{"unknown object type", s};
                    }
                    ++s;

                    if (s == e) {
                        throw opl_error{"expected integer", s};
                    }
                    osmium::object_id_type ref = opl_parse_id(&s);
                    opl_parse_char(&s, '@');
                    if (s == e) {
                        builder.add_member(type, ref, "");
                        return;
                    }
                    std::string role;
                    opl_parse_string(&s, role);
                    builder.add_member(type, ref, role);

                    if (s == e) {
                        return;
                    }
                    opl_parse_char(&s, ',');
                }
            }

            inline void opl_parse_relation(const char** data, osmium::memory::Buffer& buffer) {
                osmium::builder::RelationBuilder builder{buffer};

                builder.set_id(opl_parse_id(data));

                const char* tags_begin = nullptr;

                const char* members_begin = nullptr;
                const char* members_end = nullptr;

                bool has_version = false;
                bool has_visible = false;
                bool has_changeset_id = false;
                bool has_timestamp = false;
                bool has_uid = false;
                bool has_user = false;
                bool has_tags = false;
                bool has_members = false;

                std::string user;
                while (**data) {
                    opl_parse_space(data);
                    const char c = **data;
                    if (c == '\0') {
                        break;
                    }
                    ++(*data);
                    switch (c) {
                        case 'v':
                            if (has_version) {
                                throw opl_error{"Duplicate attribute: version (v)"};
                            }
                            has_version = true;
                            builder.set_version(opl_parse_version(data));
                            break;
                        case 'd':
                            if (has_visible) {
                                throw opl_error{"Duplicate attribute: visible (d)"};
                            }
                            has_visible = true;
                            builder.set_visible(opl_parse_visible(data));
                            break;
                        case 'c':
                            if (has_changeset_id) {
                                throw opl_error{"Duplicate attribute: changeset_id (c)"};
                            }
                            has_changeset_id = true;
                            builder.set_changeset(opl_parse_changeset_id(data));
                            break;
                        case 't':
                            if (has_timestamp) {
                                throw opl_error{"Duplicate attribute: timestamp (t)"};
                            }
                            has_timestamp = true;
                            builder.set_timestamp(opl_parse_timestamp(data));
                            break;
                        case 'i':
                            if (has_uid) {
                                throw opl_error{"Duplicate attribute: uid (i)"};
                            }
                            has_uid = true;
                            builder.set_uid(opl_parse_uid(data));
                            break;
                        case 'u':
                            if (has_user) {
                                throw opl_error{"Duplicate attribute: user (u)"};
                            }
                            has_user = true;
                            opl_parse_string(data, user);
                            break;
                        case 'T':
                            if (has_tags) {
                                throw opl_error{"Duplicate attribute: tags (T)"};
                            }
                            has_tags = true;
                            if (opl_non_empty(*data)) {
                                tags_begin = *data;
                                opl_skip_section(data);
                            }
                            break;
                        case 'M':
                            if (has_members) {
                                throw opl_error{"Duplicate attribute: members (M)"};
                            }
                            has_members = true;
                            members_begin = *data;
                            members_end = opl_skip_section(data);
                            break;
                        default:
                            --(*data);
                            throw opl_error{"unknown attribute", *data};
                    }
                }

                builder.set_user(user);

                if (tags_begin) {
                    opl_parse_tags(tags_begin, buffer, &builder);
                }

                if (members_begin != members_end) {
                    opl_parse_relation_members(members_begin, members_end, buffer, &builder);
                }
            }

            inline void opl_parse_changeset(const char** data, osmium::memory::Buffer& buffer) {
                osmium::builder::ChangesetBuilder builder{buffer};

                builder.set_id(opl_parse_changeset_id(data));

                const char* tags_begin = nullptr;

                bool has_num_changes = false;
                bool has_created_at = false;
                bool has_closed_at = false;
                bool has_num_comments = false;
                bool has_uid = false;
                bool has_user = false;
                bool has_tags = false;
                bool has_min_x = false;
                bool has_min_y = false;
                bool has_max_x = false;
                bool has_max_y = false;

                osmium::Box box;
                std::string user;
                while (**data) {
                    opl_parse_space(data);
                    const char c = **data;
                    if (c == '\0') {
                        break;
                    }
                    ++(*data);
                    switch (c) {
                        case 'k':
                            if (has_num_changes) {
                                throw opl_error{"Duplicate attribute: num_changes (k)"};
                            }
                            has_num_changes = true;
                            builder.set_num_changes(opl_parse_int<osmium::num_changes_type>(data));
                            break;
                        case 's':
                            if (has_created_at) {
                                throw opl_error{"Duplicate attribute: created_at (s)"};
                            }
                            has_created_at = true;
                            builder.set_created_at(opl_parse_timestamp(data));
                            break;
                        case 'e':
                            if (has_closed_at) {
                                throw opl_error{"Duplicate attribute: closed_at (e)"};
                            }
                            has_closed_at = true;
                            builder.set_closed_at(opl_parse_timestamp(data));
                            break;
                        case 'd':
                            if (has_num_comments) {
                                throw opl_error{"Duplicate attribute: num_comments (d)"};
                            }
                            has_num_comments = true;
                            builder.set_num_comments(opl_parse_int<osmium::num_comments_type>(data));
                            break;
                        case 'i':
                            if (has_uid) {
                                throw opl_error{"Duplicate attribute: uid (i)"};
                            }
                            has_uid = true;
                            builder.set_uid(opl_parse_uid(data));
                            break;
                        case 'u':
                            if (has_user) {
                                throw opl_error{"Duplicate attribute: user (u)"};
                            }
                            has_user = true;
                            opl_parse_string(data, user);
                            break;
                        case 'x':
                            if (has_min_x) {
                                throw opl_error{"Duplicate attribute: min_x (x)"};
                            }
                            has_min_x = true;
                            if (opl_non_empty(*data)) {
                                box.bottom_left().set_lon_partial(data);
                            }
                            break;
                        case 'y':
                            if (has_min_y) {
                                throw opl_error{"Duplicate attribute: min_y (y)"};
                            }
                            has_min_y = true;
                            if (opl_non_empty(*data)) {
                                box.bottom_left().set_lat_partial(data);
                            }
                            break;
                        case 'X':
                            if (has_max_x) {
                                throw opl_error{"Duplicate attribute: max_x (X)"};
                            }
                            has_max_x = true;
                            if (opl_non_empty(*data)) {
                                box.top_right().set_lon_partial(data);
                            }
                            break;
                        case 'Y':
                            if (has_max_y) {
                                throw opl_error{"Duplicate attribute: max_y (Y)"};
                            }
                            has_max_y = true;
                            if (opl_non_empty(*data)) {
                                box.top_right().set_lat_partial(data);
                            }
                            break;
                        case 'T':
                            if (has_tags) {
                                throw opl_error{"Duplicate attribute: tags (T)"};
                            }
                            has_tags = true;
                            if (opl_non_empty(*data)) {
                                tags_begin = *data;
                                opl_skip_section(data);
                            }
                            break;
                        default:
                            --(*data);
                            throw opl_error{"unknown attribute", *data};
                    }

                }

                builder.set_bounds(box);
                builder.set_user(user);

                if (tags_begin) {
                    opl_parse_tags(tags_begin, buffer, &builder);
                }
            }

            inline bool opl_parse_line(uint64_t line_count,
                                       const char* data,
                                       osmium::memory::Buffer& buffer,
                                       osmium::osm_entity_bits::type read_types = osmium::osm_entity_bits::all) {
                const char* start_of_line = data;
                try {
                    switch (*data) {
                        case '\0':
                            // ignore empty lines
                            break;
                        case '#':
                            // ignore lines starting with #
                            break;
                        case 'n':
                            if (read_types & osmium::osm_entity_bits::node) {
                                ++data;
                                opl_parse_node(&data, buffer);
                                buffer.commit();
                                return true;
                            }
                            break;
                        case 'w':
                            if (read_types & osmium::osm_entity_bits::way) {
                                ++data;
                                opl_parse_way(&data, buffer);
                                buffer.commit();
                                return true;
                            }
                            break;
                        case 'r':
                            if (read_types & osmium::osm_entity_bits::relation) {
                                ++data;
                                opl_parse_relation(&data, buffer);
                                buffer.commit();
                                return true;
                            }
                            break;
                        case 'c':
                            if (read_types & osmium::osm_entity_bits::changeset) {
                                ++data;
                                opl_parse_changeset(&data, buffer);
                                buffer.commit();
                                return true;
                            }
                            break;
                        default:
                            throw opl_error{"unknown type", data};
                    }
                } catch (opl_error& e) {
                    e.set_pos(line_count, e.data ? e.data - start_of_line : 0);
                    throw;
                }

                return false;
            }

        } // namespace detail

    } // namespace io

} // namespace osmium


#endif // OSMIUM_IO_DETAIL_OPL_PARSER_FUNCTIONS_HPP
