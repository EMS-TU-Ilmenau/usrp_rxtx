// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

#include <string>

#include "test.hpp"
#include "../src/json.hpp"

// test object
static const Json::Object object {{
    { "str", "str\n\t\r\vstr" },
    { "int", -42 },
    { "uint", 42ul },
    { "float", 0.0078125f },
    { "double0", 0. },
    { "double1", 3.141592653589793 },
    { "bool", true },
    { "null", Json::Null{} },
    { "array", Json::Array{{
        0, 1., "str", true, Json::Array{}, Json::Object{}
    }} },
    { "object", Json::Object{{
        { "num", 42 },
        { "bool", true },
        { "array", Json::Array{{0, 1, 2, 3}} },
        { "object", Json::Object{{{"num", 42}, {"str", "str"}}} }
    }} },
}};

// above test object serialized as JSON
static const std::string serialized{"{\"str\": \"str\\n\\t\\r\\u000bstr\", \"int\": -42, \"uint\": 42, \"float\": 0.0078125, \"double0\": 0, \"double1\": 3.14159265358979, \"bool\": true, \"null\": null, \"array\": [0, 1, \"str\", true, [], {}], \"object\": {\"num\": 42, \"bool\": true, \"array\": [0, 1, 2, 3], \"object\": {\"num\": 42, \"str\": \"str\"}}}"};

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    if (object.dumps() != serialized)
        throw test_error{"Json serialization result invalid"};
    if (Json{object}.dumps() != serialized)
        throw test_error{"Json serialization result invalid"};

    return 0;
}
