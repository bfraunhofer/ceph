// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2014 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>

#include "crush/CrushWrapper.h"
#include "common/config.h"
#include "include/stringify.h"
#include "global/global_init.h"
#include "erasure-code/LRC/ErasureCodeLRC.h"
#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "gtest/gtest.h"

TEST(ErasureCodeLRC, parse_ruleset)
{
  ErasureCodeLRC LRC;
  EXPECT_EQ("default", LRC.ruleset_root);
  EXPECT_EQ("host", LRC.ruleset_steps.front().type);

  map<std::string,std::string> parameters;
  parameters["ruleset-root"] = "other";
  EXPECT_EQ(0, LRC.parse_ruleset(parameters, &cerr));
  EXPECT_EQ("other", LRC.ruleset_root);

  parameters["ruleset-steps"] = "[]";
  EXPECT_EQ(0, LRC.parse_ruleset(parameters, &cerr));
  EXPECT_TRUE(LRC.ruleset_steps.empty());

  parameters["ruleset-steps"] = "0";
  EXPECT_EQ(ERROR_LRC_ARRAY, LRC.parse_ruleset(parameters, &cerr));

  parameters["ruleset-steps"] = "{";
  EXPECT_EQ(ERROR_LRC_PARSE_JSON, LRC.parse_ruleset(parameters, &cerr));

  parameters["ruleset-steps"] = "[0]";
  EXPECT_EQ(ERROR_LRC_ARRAY, LRC.parse_ruleset(parameters, &cerr));

  parameters["ruleset-steps"] = "[[0]]";
  EXPECT_EQ(ERROR_LRC_RULESET_OP, LRC.parse_ruleset(parameters, &cerr));

  parameters["ruleset-steps"] = "[[\"choose\", 0]]";
  EXPECT_EQ(ERROR_LRC_RULESET_TYPE, LRC.parse_ruleset(parameters, &cerr));

  parameters["ruleset-steps"] = "[[\"choose\", \"host\", []]]";
  EXPECT_EQ(ERROR_LRC_RULESET_N, LRC.parse_ruleset(parameters, &cerr));

  parameters["ruleset-steps"] = "[[\"choose\", \"host\", 2]]";
  EXPECT_EQ(0, LRC.parse_ruleset(parameters, &cerr));

  const ErasureCodeLRC::Step &step = LRC.ruleset_steps.front();
  EXPECT_EQ("choose", step.op);
  EXPECT_EQ("host", step.type);
  EXPECT_EQ(2, step.n);

  parameters["ruleset-steps"] =
    "["
    " [\"choose\", \"rack\", 2], "
    " [\"chooseleaf\", \"host\", 5], "
    "]";
  EXPECT_EQ(0, LRC.parse_ruleset(parameters, &cerr));
  EXPECT_EQ(2U, LRC.ruleset_steps.size());
  {
    const ErasureCodeLRC::Step &step = LRC.ruleset_steps[0];
    EXPECT_EQ("choose", step.op);
    EXPECT_EQ("rack", step.type);
    EXPECT_EQ(2, step.n);
  }
  {
    const ErasureCodeLRC::Step &step = LRC.ruleset_steps[1];
    EXPECT_EQ("chooseleaf", step.op);
    EXPECT_EQ("host", step.type);
    EXPECT_EQ(5, step.n);
  }
}

TEST(ErasureCodeTest, create_ruleset)
{
  CrushWrapper *c = new CrushWrapper;
  c->create();
  int root_type = 3;
  c->set_type_name(root_type, "root");
  int rack_type = 2;
  c->set_type_name(rack_type, "rack");
  int host_type = 1;
  c->set_type_name(host_type, "host");
  int osd_type = 0;
  c->set_type_name(osd_type, "osd");

  int rootno;
  c->add_bucket(0, CRUSH_BUCKET_STRAW, CRUSH_HASH_RJENKINS1,
		root_type, 0, NULL, NULL, &rootno);
  c->set_item_name(rootno, "default");

  map<string,string> loc;
  loc["root"] = "default";

  //
  // Set all to 10 so that the item number it trivial to decompose
  // into rack/host/osd.
  //
  int num_rack;
  int num_host;
  int num_osd;
  num_rack = num_host = num_osd = 10;
  int osd = 0;
  for (int r=0; r<num_rack; ++r) {
    loc["rack"] = string("rack-") + stringify(r);
    for (int h=0; h<num_host; ++h) {
      loc["host"] = string("host-") + stringify(r) + string("-") + stringify(h);
      for (int o=0; o<num_osd; ++o, ++osd) {
	c->insert_item(g_ceph_context, osd, 1.0, string("osd.") + stringify(osd), loc);
      }
    }
  }

  ErasureCodeLRC LRC;
  EXPECT_EQ(0, LRC.create_ruleset("rule1", *c, &cerr));

  map<std::string,std::string> parameters;
  unsigned int racks = 2;
  unsigned int hosts = 5;
  parameters["ruleset-steps"] =
    "["
    " [\"choose\", \"rack\", " + stringify(racks) + "], "
    " [\"chooseleaf\", \"host\", " + stringify(hosts) + "], "
    "]";
  const char *rule_name = "rule2";
  EXPECT_EQ(0, LRC.parse_ruleset(parameters, &cerr));
  EXPECT_EQ(1, LRC.create_ruleset(rule_name, *c, &cerr));

  vector<__u32> weight;
  for (int o = 0; o < c->get_max_devices(); o++)
    weight.push_back(0x10000);
  int rule = c->get_rule_id(rule_name);
  vector<int> out;
  unsigned int n = racks * hosts;
  c->do_rule(rule, 1, out, n, weight);
  EXPECT_EQ(n, out.size());
  //
  // check that the first five are in the same rack and the next five
  // in the same rack
  //
  int first_rack = out[0] / num_host / num_osd;
  EXPECT_EQ(first_rack, out[1] / num_host / num_osd);
  EXPECT_EQ(first_rack, out[2] / num_host / num_osd);
  EXPECT_EQ(first_rack, out[3] / num_host / num_osd);
  EXPECT_EQ(first_rack, out[4] / num_host / num_osd);
  int second_rack = out[5] / num_host / num_osd;
  EXPECT_EQ(second_rack, out[6] / num_host / num_osd);
  EXPECT_EQ(second_rack, out[7] / num_host / num_osd);
  EXPECT_EQ(second_rack, out[8] / num_host / num_osd);
  EXPECT_EQ(second_rack, out[9] / num_host / num_osd);
}

TEST(ErasureCodeLRC, parse_kml)
{
  ErasureCodeLRC LRC;
  map<std::string,std::string> parameters;
  EXPECT_EQ(0, LRC.parse_kml(parameters, &cerr));
  parameters["k"] = "4";
  EXPECT_EQ(ERROR_LRC_ALL_OR_NOTHING, LRC.parse_kml(parameters, &cerr));
  const char *generated[] = { "mapping",
			      "layers",
			      "ruleset-steps" };
  parameters["m"] = "2";
  parameters["l"] = "3";

  for (int i = 0; i < 3; i++) {
    parameters[generated[i]] = "SET";
    EXPECT_EQ(ERROR_LRC_GENERATED, LRC.parse_kml(parameters, &cerr));
    parameters.erase(parameters.find(generated[i]));
  }

  parameters["k"] = "4";
  parameters["m"] = "2";
  parameters["l"] = "7";
  EXPECT_EQ(ERROR_LRC_K_M_MODULO, LRC.parse_kml(parameters, &cerr));

  parameters["k"] = "3";
  parameters["m"] = "3";
  parameters["l"] = "3";
  EXPECT_EQ(ERROR_LRC_K_MODULO, LRC.parse_kml(parameters, &cerr));

  parameters["k"] = "4";
  parameters["m"] = "2";
  parameters["l"] = "3";
  EXPECT_EQ(0, LRC.parse_kml(parameters, &cerr));
  EXPECT_EQ("[ "
	    " [ \"DDc_DDc_\", \"\" ],"
	    " [ \"DDDc____\", \"\" ],"
	    " [ \"____DDDc\", \"\" ],"
	    "]", parameters["layers"]);
  EXPECT_EQ("DD__DD__", parameters["mapping"]);
  EXPECT_EQ("chooseleaf", LRC.ruleset_steps[0].op);
  EXPECT_EQ("host", LRC.ruleset_steps[0].type);
  EXPECT_EQ(0, LRC.ruleset_steps[0].n);
  EXPECT_EQ(1U, LRC.ruleset_steps.size());
  parameters.erase(parameters.find("mapping"));
  parameters.erase(parameters.find("layers"));

  parameters["k"] = "4";
  parameters["m"] = "2";
  parameters["l"] = "3";
  parameters["ruleset-failure-domain"] = "osd";
  EXPECT_EQ(0, LRC.parse_kml(parameters, &cerr));  
  EXPECT_EQ("chooseleaf", LRC.ruleset_steps[0].op);
  EXPECT_EQ("osd", LRC.ruleset_steps[0].type);
  EXPECT_EQ(0, LRC.ruleset_steps[0].n);
  EXPECT_EQ(1U, LRC.ruleset_steps.size());
  parameters.erase(parameters.find("mapping"));
  parameters.erase(parameters.find("layers"));

  parameters["k"] = "4";
  parameters["m"] = "2";
  parameters["l"] = "3";
  parameters["ruleset-failure-domain"] = "osd";
  parameters["ruleset-locality"] = "rack";
  EXPECT_EQ(0, LRC.parse_kml(parameters, &cerr));  
  EXPECT_EQ("choose", LRC.ruleset_steps[0].op);
  EXPECT_EQ("rack", LRC.ruleset_steps[0].type);
  EXPECT_EQ(2, LRC.ruleset_steps[0].n);
  EXPECT_EQ("chooseleaf", LRC.ruleset_steps[1].op);
  EXPECT_EQ("osd", LRC.ruleset_steps[1].type);
  EXPECT_EQ(4, LRC.ruleset_steps[1].n);
  EXPECT_EQ(2U, LRC.ruleset_steps.size());
  parameters.erase(parameters.find("mapping"));
  parameters.erase(parameters.find("layers"));
}

TEST(ErasureCodeLRC, layers_description)
{
  ErasureCodeLRC LRC;
  map<std::string,std::string> parameters;

  json_spirit::mArray description;
  EXPECT_EQ(ERROR_LRC_DESCRIPTION,
	    LRC.layers_description(parameters, &description, &cerr));

  {
    const char *description_string = "\"not an array\"";
    parameters["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_ARRAY,
	      LRC.layers_description(parameters, &description, &cerr));
  }
  {
    const char *description_string = "invalid json";
    parameters["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_PARSE_JSON,
	      LRC.layers_description(parameters, &description, &cerr));
  }
  {
    const char *description_string = "[]";
    parameters["layers"] = description_string;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
  }
}

TEST(ErasureCodeLRC, layers_parse)
{
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;

    const char *description_string ="[ 0 ]";
    parameters["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
    EXPECT_EQ(ERROR_LRC_ARRAY,
	      LRC.layers_parse(description_string, description, &cerr));
  }

  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;

    const char *description_string ="[ [ 0 ] ]";
    parameters["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
    EXPECT_EQ(ERROR_LRC_STR,
	      LRC.layers_parse(description_string, description, &cerr));
  }

  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;

    const char *description_string ="[ [ \"\", 0 ] ]";
    parameters["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
    EXPECT_EQ(ERROR_LRC_CONFIG_OPTIONS,
	      LRC.layers_parse(description_string, description, &cerr));
  }

  //
  // The second element can be an object describing the plugin
  // parameters.
  //
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;

    const char *description_string ="[ [ \"\", { \"a\": \"b\" }, \"ignored\" ] ]";
    parameters["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
    EXPECT_EQ(0, LRC.layers_parse(description_string, description, &cerr));
    EXPECT_EQ("b", LRC.layers.front().parameters["a"]);
  }

  //
  // The second element can be a str_map parseable string describing the plugin
  // parameters.
  //
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;

    const char *description_string ="[ [ \"\", \"a=b c=d\" ] ]";
    parameters["layers"] = description_string;
    json_spirit::mArray description;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
    EXPECT_EQ(0, LRC.layers_parse(description_string, description, &cerr));
    EXPECT_EQ("b", LRC.layers.front().parameters["a"]);
    EXPECT_EQ("d", LRC.layers.front().parameters["c"]);
  }

}

TEST(ErasureCodeLRC, layers_sanity_checks)
{
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    parameters["mapping"] =
	    "__DDD__DD";
    parameters["directory"] = ".libs";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD\", \"\" ],"
      "  [ \"c_DDD____\", \"\" ],"
      "  [ \"_____cDDD\", \"\" ],"
      "]";
    parameters["layers"] = description_string;
    EXPECT_EQ(0, LRC.init(parameters, &cerr));
  }
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    const char *description_string =
      "[ "
      "]";
    parameters["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_MAPPING, LRC.init(parameters, &cerr));
  }
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    parameters["mapping"] = "";
    const char *description_string =
      "[ "
      "]";
    parameters["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_LAYERS_COUNT, LRC.init(parameters, &cerr));
  }
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    parameters["directory"] = ".libs";
    parameters["mapping"] =
	    "AA";
    const char *description_string =
      "[ "
      "  [ \"AA??\", \"\" ], "
      "  [ \"AA\", \"\" ], "
      "  [ \"AA\", \"\" ], "
      "]";
    parameters["layers"] = description_string;
    EXPECT_EQ(ERROR_LRC_MAPPING_SIZE, LRC.init(parameters, &cerr));
  }
}

TEST(ErasureCodeLRC, layers_init)
{
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;

    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD_\", \"directory=.libs\" ],"
      "]";
    parameters["layers"] = description_string;
    parameters["directory"] = ".libs";
    json_spirit::mArray description;
    EXPECT_EQ(0, LRC.layers_description(parameters, &description, &cerr));
    EXPECT_EQ(0, LRC.layers_parse(description_string, description, &cerr));
    EXPECT_EQ(0, LRC.layers_init());
    EXPECT_EQ("5", LRC.layers.front().parameters["k"]);
    EXPECT_EQ("2", LRC.layers.front().parameters["m"]);
    EXPECT_EQ("jerasure", LRC.layers.front().parameters["plugin"]);
    EXPECT_EQ("reed_sol_van", LRC.layers.front().parameters["technique"]);
  }
}

TEST(ErasureCodeLRC, init)
{
  ErasureCodeLRC LRC;
  map<std::string,std::string> parameters;
  parameters["mapping"] =
    "__DDD__DD";
  const char *description_string =
    "[ "
    "  [ \"_cDDD_cDD\", \"\" ],"
    "  [ \"c_DDD____\", \"\" ],"
    "  [ \"_____cDDD\", \"\" ],"
    "]";
  parameters["layers"] = description_string;
  parameters["directory"] = ".libs";
  EXPECT_EQ(0, LRC.init(parameters, &cerr));
}

TEST(ErasureCodeLRC, init_kml)
{
  ErasureCodeLRC LRC;
  map<std::string,std::string> parameters;
  parameters["k"] = "4";
  parameters["m"] = "2";
  parameters["l"] = "3";
  parameters["directory"] = ".libs";
  EXPECT_EQ(0, LRC.init(parameters, &cerr));
  EXPECT_EQ((unsigned int)(4 + 2 + (4 + 2) / 3), LRC.get_chunk_count());
}

TEST(ErasureCodeLRC, minimum_to_decode)
{
  // trivial : no erasures, the minimum is want_to_read
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    parameters["mapping"] =
      "__DDD__DD";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD\", \"\" ],"
      "  [ \"c_DDD____\", \"\" ],"
      "  [ \"_____cDDD\", \"\" ],"
      "]";
    parameters["layers"] = description_string;
    parameters["directory"] = ".libs";
    EXPECT_EQ(0, LRC.init(parameters, &cerr));
    set<int> want_to_read;
    want_to_read.insert(1);
    set<int> available_chunks;
    available_chunks.insert(1);
    available_chunks.insert(2);
    set<int> minimum;
    EXPECT_EQ(0, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(want_to_read, minimum);
  }
  // locally repairable erasure
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    parameters["mapping"] =
	    "__DDD__DD_";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD_\", \"\" ],"
      "  [ \"c_DDD_____\", \"\" ],"
      "  [ \"_____cDDD_\", \"\" ],"
      "  [ \"_____DDDDc\", \"\" ],"
      "]";
    parameters["layers"] = description_string;
    parameters["directory"] = ".libs";
    EXPECT_EQ(0, LRC.init(parameters, &cerr));
    EXPECT_EQ(parameters["mapping"].length(),
	      LRC.get_chunk_count());
    {
      // want to read the last chunk
      set<int> want_to_read;
      want_to_read.insert(LRC.get_chunk_count() - 1);
      // all chunks are available except the last chunk
      set<int> available_chunks;
      for (int i = 0; i < (int)LRC.get_chunk_count() - 1; i++)
	available_chunks.insert(i);
      // _____DDDDc can recover c
      set<int> minimum;
      EXPECT_EQ(0, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
      set<int> expected_minimum;
      expected_minimum.insert(5);
      expected_minimum.insert(6);
      expected_minimum.insert(7);
      expected_minimum.insert(8);
      EXPECT_EQ(expected_minimum, minimum);
    }
    {
      set<int> want_to_read;
      want_to_read.insert(0);
      set<int> available_chunks;
      for (int i = 1; i < (int)LRC.get_chunk_count(); i++)
	available_chunks.insert(i);
      set<int> minimum;
      EXPECT_EQ(0, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
      set<int> expected_minimum;
      expected_minimum.insert(2);
      expected_minimum.insert(3);
      expected_minimum.insert(4);
      EXPECT_EQ(expected_minimum, minimum);
    }
  }
  // implicit parity required
  {
    ErasureCodeLRC LRC;
    map<std::string,std::string> parameters;
    parameters["mapping"] =
	    "__DDD__DD";
    const char *description_string =
      "[ "
      "  [ \"_cDDD_cDD\", \"\" ],"
      "  [ \"c_DDD____\", \"\" ],"
      "  [ \"_____cDDD\", \"\" ],"
      "]";
    parameters["layers"] = description_string;
    parameters["directory"] = ".libs";
    EXPECT_EQ(0, LRC.init(parameters, &cerr));
    EXPECT_EQ(parameters["mapping"].length(),
	      LRC.get_chunk_count());
    set<int> want_to_read;
    want_to_read.insert(8);
    //
    // unable to recover, too many chunks missing
    //
    {
      set<int> available_chunks;
      available_chunks.insert(0);
      available_chunks.insert(1);
      // missing             (2)
      // missing             (3)
      available_chunks.insert(4);
      available_chunks.insert(5);
      available_chunks.insert(6);
      // missing             (7)
      // missing             (8)
      set<int> minimum;
      EXPECT_EQ(-EIO, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
    }
    //
    // We want to read chunk 8 and encoding was done with
    //
    //     _cDDD_cDD
    //	   c_DDD____
    //	   _____cDDD
    //
    // First strategy fails:
    //
    // 012345678
    // xxXXXxxXX  initial chunks
    // xx.XXxx..  missing (2, 7, 8)
    // _____cDDD  fail : can recover 1 but 2 are missing
    // c_DDD____  ignored because 8 is not used (i.e. _)
    // _cDDD_cDD  fail : can recover 2 but 3 are missing
    //
    // Second strategy succeeds:
    //
    // 012345678
    // xxXXXxxXX  initial chunks
    // xx.XXxx..  missing (2, 7, 8)
    // _____cDDD  fail : can recover 1 but 2 are missing
    // c_DDD____  success: recovers chunk 2
    // _cDDD_cDD  success: recovers chunk 7, 8
    //
    {
      set<int> available_chunks;
      available_chunks.insert(0);
      available_chunks.insert(1);
      // missing             (2)
      available_chunks.insert(3);
      available_chunks.insert(4);
      available_chunks.insert(5);
      available_chunks.insert(6);
      // missing             (7)
      // missing             (8)
      set<int> minimum;
      EXPECT_EQ(0, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
      EXPECT_EQ(available_chunks, minimum);
    }
  }
}

TEST(ErasureCodeLRC, encode_decode)
{
  ErasureCodeLRC LRC;
  map<std::string,std::string> parameters;
  parameters["mapping"] =
    "__DD__DD";
  const char *description_string =
    "[ "
    "  [ \"_cDD_cDD\", \"\" ]," // global layer 
    "  [ \"c_DD____\", \"\" ]," // first local layer 
    "  [ \"____cDDD\", \"\" ]," // second local layer 
    "]";
  parameters["layers"] = description_string;
  parameters["directory"] = ".libs";
  EXPECT_EQ(0, LRC.init(parameters, &cerr));
  EXPECT_EQ(4U, LRC.get_data_chunk_count());
  unsigned int stripe_width = g_conf->osd_pool_erasure_code_stripe_width;
  unsigned int chunk_size = stripe_width / LRC.get_data_chunk_count();
  EXPECT_EQ(chunk_size, LRC.get_chunk_size(stripe_width));
  set<int> want_to_encode;
  map<int, bufferlist> encoded;
  for (unsigned int i = 0; i < LRC.get_chunk_count(); ++i) {
    want_to_encode.insert(i);
    bufferptr ptr(buffer::create_page_aligned(chunk_size));
    encoded[i].push_front(ptr);
  }
  const vector<int> &mapping = LRC.get_chunk_mapping();
  char c = 'A';
  for (unsigned int i = 0; i < LRC.get_data_chunk_count(); i++) {
    int j = mapping[i];
    string s(chunk_size, c);
    encoded[j].clear();
    encoded[j].append(s);
    c++;
  }
  EXPECT_EQ(0, LRC.encode_chunks(want_to_encode, &encoded));

  {
    map<int, bufferlist> chunks;
    chunks[4] = encoded[4];
    chunks[5] = encoded[5];
    chunks[6] = encoded[6];
    set<int> want_to_read;
    want_to_read.insert(7);
    set<int> available_chunks;
    available_chunks.insert(4);
    available_chunks.insert(5);
    available_chunks.insert(6);
    set<int> minimum;
    EXPECT_EQ(0, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
    // only need three chunks from the second local layer
    EXPECT_EQ(3U, minimum.size()); 
    EXPECT_EQ(1U, minimum.count(4));
    EXPECT_EQ(1U, minimum.count(5));
    EXPECT_EQ(1U, minimum.count(6));
    map<int, bufferlist> decoded;
    EXPECT_EQ(0, LRC.decode(want_to_read, chunks, &decoded));
    string s(chunk_size, 'D');
    EXPECT_EQ(s, string(decoded[7].c_str(), chunk_size));
  }
  {
    set<int> want_to_read;
    want_to_read.insert(3);
    want_to_read.insert(6);
    want_to_read.insert(7);
    set<int> available_chunks;
    available_chunks.insert(0);
    available_chunks.insert(1);
    available_chunks.insert(2);
    // available_chunks.insert(3);
    available_chunks.insert(4);
    available_chunks.insert(5);
    // available_chunks.insert(6);
    // available_chunks.insert(7);
    encoded.erase(3);
    encoded.erase(6);
    set<int> minimum;
    EXPECT_EQ(0, LRC.minimum_to_decode(want_to_read, available_chunks, &minimum));
    EXPECT_EQ(4U, minimum.size()); 
    // only need two chunks from the first local layer
    EXPECT_EQ(1U, minimum.count(0));
    EXPECT_EQ(1U, minimum.count(2));
    // the above chunks will rebuild chunk 3 and the global layer only needs
    // three more chunks to reach the required amount of chunks (4) to recover
    // the last two
    EXPECT_EQ(1U, minimum.count(1));
    EXPECT_EQ(1U, minimum.count(2));
    EXPECT_EQ(1U, minimum.count(5));

    map<int, bufferlist> decoded;
    EXPECT_EQ(0, LRC.decode(want_to_read, encoded, &decoded));
    {
      string s(chunk_size, 'B');
      EXPECT_EQ(s, string(decoded[3].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'C');
      EXPECT_EQ(s, string(decoded[6].c_str(), chunk_size));
    }
    {
      string s(chunk_size, 'D');
      EXPECT_EQ(s, string(decoded[7].c_str(), chunk_size));
    }
  }

}

int main(int argc, char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/*
 * Local Variables:
 * compile-command: "cd ../.. ;
 *   make -j4 unittest_erasure_code_LRC &&
 *   valgrind --tool=memcheck --leak-check=full \
 *      ./unittest_erasure_code_LRC \
 *      --gtest_filter=*.* --log-to-stderr=true --debug-osd=20"
 * End:
 */
