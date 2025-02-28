// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#if defined(BORINGSSL_FIPS)

#include <gtest/gtest.h>

#include "../../test/test_util.h"
#include "../../../third_party/jitterentropy/jitterentropy.h"

// Struct for Jitter entropy collector instance with constructor/desctructor.
struct JitterEC {
  rand_data *instance;

  JitterEC(unsigned int osr, unsigned int flags) {
    instance = nullptr;
    instance = jent_entropy_collector_alloc(osr, flags);
  }

  ~JitterEC() {
    jent_entropy_collector_free(instance);
    instance = nullptr;
  }
};

TEST(CPUJitterEntropyTest, Basic) {

  // Allocate Jitter instance with default oversampling rate.
  JitterEC jitter_ec(0, JENT_FORCE_FIPS);

  // Check that the instance is properly allocated and initialized.
  EXPECT_NE(jitter_ec.instance, nullptr);

  // Check that the default oversampling rate is 3 as expected.
  unsigned int default_osr = 3;
  EXPECT_EQ(jitter_ec.instance->osr, default_osr);

  const ssize_t data_len = 48;
  uint8_t data0[data_len], data1[data_len];

  // Draw some entropy to check if it works.
  EXPECT_EQ(jent_read_entropy(jitter_ec.instance,
                              (char*) data0, data_len), data_len);

  // Draw some entropy with the "safe" API to check if it works.
  EXPECT_EQ(jent_read_entropy_safe(&jitter_ec.instance,
                                   (char*) data1, data_len), data_len);

  // Basic check that the random data is not equal.
  EXPECT_NE(Bytes(data0), Bytes(data1));

  // Free Jitter instance and initialize a new one with different osr.
  jent_entropy_collector_free(jitter_ec.instance);

  unsigned int osr = 5;
  jitter_ec.instance = jent_entropy_collector_alloc(osr, JENT_FORCE_FIPS);
  EXPECT_NE(jitter_ec.instance, nullptr);
  EXPECT_EQ(jitter_ec.instance->osr, osr);

  // Verify that the Jitter library version is v3.4.0.
  unsigned int jitter_version = 3040000;
  EXPECT_EQ(jitter_version, jent_version());
}

#endif
