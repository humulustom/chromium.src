#!/usr/bin/env vpython
# Copyright 2016 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# pylint: disable=too-many-lines
# pylint: disable=line-too-long

"""Generates chromium.perf{,.fyi}.json from a set of condensed configs.

This file contains condensed configurations for the perf bots along with
logic to inflate those into the full (unwieldy) configurations in
//testing/buildbot that are consumed by the chromium recipe code.
"""

from __future__ import print_function

import argparse
import collections
import csv
import filecmp
import json
import os
import re
import shutil
import sys
import tempfile
import textwrap

from core import benchmark_finders
from core import benchmark_utils
from core import bot_platforms
from core import path_util
from core import undocumented_benchmarks as ub_module
path_util.AddTelemetryToPath()

from telemetry import decorators


# The condensed configurations below get inflated into the perf builder
# configurations in //testing/buildbot. The expected format of these is:
#
#   {
#     'builder_name1': {
#       # Targets that the builder should compile in addition to those
#       # required for tests, as a list of strings.
#       'additional_compile_targets': ['target1', 'target2', ...],
#
#       'tests': [
#         {
#           # Arguments to pass to the test suite as a list of strings.
#           'extra_args': ['--arg1', '--arg2', ...],
#
#           # Name of the isolate to run as a string.
#           'isolate': 'isolate_name',
#
#           # Name of the test suite as a string.
#           # If not present, will default to `isolate`.
#           'name': 'presentation_name',
#
#           # The number of shards for this test as an int.
#           # This is only required for GTEST tests since this is defined
#           # in bot_platforms.py for Telemetry tests.
#           'num_shards': 2,
#
#           # What kind of test this is; for options, see TEST_TYPES
#           # below. Defaults to TELEMETRY.
#           'type': TEST_TYPES.TELEMETRY,
#         },
#         ...
#       ],
#
#       # Testing platform, as a string. Used in determining the browser
#       # argument to pass to telemetry.
#       'platform': 'platform_name',
#
#       # Dimensions to pass to swarming, as a dict of string keys & values.
#       'dimension': {
#         'dimension1_name': 'dimension1_value',
#         ...
#       },
#     },
#     ...
#   }

class TEST_TYPES(object):
  GENERIC = 0
  GTEST = 1
  TELEMETRY = 2

  ALL = (GENERIC, GTEST, TELEMETRY)


FYI_BUILDERS = {
  'android-nexus5x-perf-fyi': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
          '--output-format=histograms',
        ],
      }
    ],
    'platform': 'android-chrome',
    'dimension': {
      'pool': 'chrome.tests.perf-fyi',
      'os': 'Android',
      'device_type': 'bullhead',
      'device_os': 'MMB29Q',
      'device_os_flavor': 'google',
    },
  },
  'android-pixel2-perf-fyi': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
      }
    ],
    'platform': 'android-chrome',
    'browser': 'bin/monochrome_64_32_bundle',
    'dimension': {
      'pool': 'chrome.tests.perf-fyi',
      'os': 'Android',
      'device_type': 'walleye',
      'device_os': 'O',
      'device_os_flavor': 'google',
    },
  },
  'android-pixel2-perf-aab-fyi': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
           '--run-ref-build',
        ],
      }
    ],
    'platform': 'android-chrome-bundle',
    'dimension': {
      'pool': 'chrome.tests.perf-fyi',
      'os': 'Android',
      'device_type': 'walleye',
      'device_os': 'O',
      'device_os_flavor': 'google',
    },
  },
  'linux-perf-fyi': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--output-format=histograms',
            '--experimental-tbmv3-metrics',
        ],
      }
    ],
    'platform': 'linux',
    'dimension': {
      'gpu': '10de',
      'id': 'build186-b7',
      'os': 'Ubuntu-14.04',
      'pool': 'chrome.tests.perf-fyi',
    },
  },
  'win-10_laptop_low_end-perf_HP-Candidate': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
      },
    ],
    'platform': 'win',
    'target_bits': 64,
    'dimension': {
      'pool': 'chrome.tests.perf-fyi',
      'id': 'build370-a7',
      # TODO(crbug.com/971204): Explicitly set the gpu to None to make
      # chromium_swarming recipe_module ignore this dimension.
      'gpu': None,
      'os': 'Windows-10',
    },
  },
  'chromeos-kevin-perf-fyi': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            # The magic hostname that resolves to a CrOS device in the test lab
            '--remote=variable_chromeos_device_hostname',
        ],
      },
    ],
    'platform': 'chromeos',
    'target_bits': 32,
    'dimension': {
      'pool': 'luci.chrome.cros-dut',
      # TODO(crbug.com/971204): Explicitly set the gpu to None to make
      # chromium_swarming recipe_module ignore this dimension.
      'gpu': None,
      'os': 'ChromeOS',
      'device_type': 'kevin',
    },
  },
}

# These configurations are taken from chromium_perf.py in
# build/scripts/slave/recipe_modules/chromium_tests and must be kept in sync
# to generate the correct json for each tester
#
# The dimensions in pinpoint configs, excluding the dimension "pool",
# must be kept in sync with the dimensions here.
# This is to make sure the same type of machines are used between waterfall
# tests and pinpoint jobs
#
# On desktop builders, chromedriver is added as an additional compile target.
# The perf waterfall builds this target for each commit, and the resulting
# ChromeDriver is archived together with Chrome for use in bisecting.
# This can be used by Chrome test team, as well as by google3 teams for
# bisecting Chrome builds with their web tests. For questions or to report
# issues, please contact johnchen@chromium.org.
BUILDERS = {
  'android-builder-perf': {
    'additional_compile_targets': [
      'microdump_stackwalk', 'angle_perftests', 'chrome_apk'
    ],
    'tests': [
      {
        'name': 'resource_sizes_chrome_apk',
        'isolate': 'resource_sizes_chrome_apk',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_chrome_public_apk',
        'isolate': 'resource_sizes_chrome_public_apk',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_monochrome_minimal_apks',
        'isolate': 'resource_sizes_monochrome_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_monochrome_public_minimal_apks',
        'isolate': 'resource_sizes_monochrome_public_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_chrome_modern_minimal_apks',
        'isolate': 'resource_sizes_chrome_modern_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_chrome_modern_public_minimal_apks',
        'isolate': 'resource_sizes_chrome_modern_public_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_system_webview_apk',
        'isolate': 'resource_sizes_system_webview_apk',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_system_webview_google_apk',
        'isolate': 'resource_sizes_system_webview_google_apk',
        'type': TEST_TYPES.GENERIC,
      },
    ],
    'dimension': {
      'os': 'Ubuntu-16.04',
      'pool': 'chrome.tests',
    },
    'perf_trigger': False,
  },
  'android_arm64-builder-perf': {
    'additional_compile_targets': [
      'microdump_stackwalk', 'angle_perftests', 'chrome_apk'
    ],
    'tests': [
      {
        'name': 'resource_sizes_chrome_public_apk',
        'isolate': 'resource_sizes_chrome_public_apk',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_monochrome_minimal_apks',
        'isolate': 'resource_sizes_monochrome_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_monochrome_public_minimal_apks',
        'isolate': 'resource_sizes_monochrome_public_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_chrome_modern_minimal_apks',
        'isolate': 'resource_sizes_chrome_modern_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_chrome_modern_public_minimal_apks',
        'isolate': 'resource_sizes_chrome_modern_public_minimal_apks',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_system_webview_apk',
        'isolate': 'resource_sizes_system_webview_apk',
        'type': TEST_TYPES.GENERIC,
      },
      {
        'name': 'resource_sizes_system_webview_google_apk',
        'isolate': 'resource_sizes_system_webview_google_apk',
        'type': TEST_TYPES.GENERIC,
      },
    ],
    'dimension': {
      'os': 'Ubuntu-16.04',
      'pool': 'chrome.tests',
    },
    'perf_trigger': False,
  },
  'linux-builder-perf': {
    'additional_compile_targets': ['chromedriver'],
  },
  'mac-builder-perf': {
    'additional_compile_targets': ['chromedriver'],
  },
  'win32-builder-perf': {
    'additional_compile_targets': ['chromedriver'],
  },
  'win64-builder-perf': {
    'additional_compile_targets': ['chromedriver'],
  },

  'android-go-perf': {
    'tests': [
      {
        'name': 'performance_test_suite',
        'isolate': 'performance_test_suite',
      }
    ],
    'platform': 'android-chrome',
    'dimension': {
      'device_os': 'OMB1.180119.001',
      'device_type': 'gobo',
      'device_os_flavor': 'google',
      'pool': 'chrome.tests.perf',
      'os': 'Android',
    },
  },
  'android-go_webview-perf': {
    'tests': [
      {
        'isolate': 'performance_webview_test_suite',
      }
    ],
    'platform': 'android-webview-google',
    'dimension': {
      'pool': 'chrome.tests.perf-webview',
      'os': 'Android',
      'device_type': 'gobo',
      'device_os': 'OMB1.180119.001',
      'device_os_flavor': 'google',
    },
  },
  'Android Nexus5 Perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'android',
    'dimension': {
      'pool': 'chrome.tests.perf',
      'os': 'Android',
      'device_type': 'hammerhead',
      'device_os': 'KOT49H',
      'device_os_flavor': 'google',
    },
  },
  'Android Nexus5X WebView Perf': {
    'tests': [
      {
        'isolate': 'performance_webview_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      }
    ],
    'platform': 'android-webview',
    'dimension': {
      'pool': 'chrome.tests.perf-webview',
      'os': 'Android',
      'device_type': 'bullhead',
      'device_os': 'MOB30K',
      'device_os_flavor': 'aosp',
    },
  },
  'android-pixel2_webview-perf': {
    'tests': [
      {
        'isolate': 'performance_webview_test_suite',
      }
    ],
    'platform': 'android-webview-google',
    'dimension': {
      'pool': 'chrome.tests.perf-webview',
      'os': 'Android',
      'device_type': 'walleye',
      'device_os': 'OPM1.171019.021',
      'device_os_flavor': 'google',
    },
  },
  'android-pixel2_weblayer-perf': {
    'tests': [
      {
        'isolate': 'performance_weblayer_test_suite',
      }
    ],
    'platform': 'android-weblayer',
    'dimension': {
      'pool': 'chrome.tests.perf-weblayer',
      'os': 'Android',
      'device_type': 'walleye',
      'device_os': 'OPM1.171019.021',
      'device_os_flavor': 'google',
    },
  },
  'android-pixel2-perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
      }
    ],
    'platform': 'android-chrome',
    'dimension': {
      'pool': 'chrome.tests.perf',
      'os': 'Android',
      'device_type': 'walleye',
      'device_os': 'OPM1.171019.021',
      'device_os_flavor': 'google',
    },
  },
  'win-10_laptop_low_end-perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'win',
    'target_bits': 64,
    'dimension': {
      'pool': 'chrome.tests.perf',
      # Explicitly set GPU driver version and Windows OS version such
      # that we can be informed if this
      # version ever changes or becomes inconsistent. It is important
      # that bots are homogeneous. See crbug.com/988045 for history.
      'os': 'Windows-10-18363.476',
      'gpu': '8086:1616-20.19.15.5070',
      # TODO(crbug.com/998161): Add synthetic product name for these.
      # They don't have this dimension yet as I am writing this CL since
      # they are since in pool 'unassigned'.
    },
  },
  'win-10-perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'win',
    'target_bits': 64,
    'dimension': {
      'pool': 'chrome.tests.perf',
      # Explicitly set GPU driver version and Windows OS version such
      # that we can be informed if this
      # version ever changes or becomes inconsistent. It is important
      # that bots are homogeneous. See crbug.com/988045 for history.
      'os': 'Windows-10-16299.309',
      'gpu': '8086:5912-23.20.16.4877',
      'synthetic_product_name': 'OptiPlex 7050 (Dell Inc.)'
    },
  },
  'Win 7 Perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
      },
    ],
    'platform': 'win',
    'target_bits': 32,
    'dimension': {
        'gpu': '102b:0532-6.1.7600.16385',
        'os': 'Windows-2008ServerR2-SP1',
        'pool': 'chrome.tests.perf',
        'synthetic_product_name': 'PowerEdge R210 II (Dell Inc.)',
    },
  },
  'Win 7 Nvidia GPU Perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'win',
    'target_bits': 64,
    'dimension': {
        'gpu': '10de:1cb3-23.21.13.8792',
        'os': 'Windows-2008ServerR2-SP1',
        'pool': 'chrome.tests.perf',
        'synthetic_product_name': 'PowerEdge R220 [01] (Dell Inc.)'
    },
  },
  'mac-10_12_laptop_low_end-perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'mac',
    'dimension': {
      'gpu': '8086:1626',
      'os': 'Mac-10.12.6',
      'pool': 'chrome.tests.perf',
      'synthetic_product_name': 'MacBookAir7,2_x86-64-i5-5350U_Intel Broadwell HD Graphics 6000_8192_APPLE SSD SM0128G'
    },
  },
  'linux-perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
            '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'linux',
    'dimension': {
      'gpu': '10de:1cb3-384.90',
      'os': 'Ubuntu-14.04',
      'pool': 'chrome.tests.perf',
      'synthetic_product_name': 'PowerEdge R230 (Dell Inc.)'
    },
  },
  'mac-10_13_laptop_high_end-perf': {
    'tests': [
      {
        'isolate': 'performance_test_suite',
        'extra_args': [
          '--assert-gpu-compositing',
        ],
      },
    ],
    'platform': 'mac',
    'dimension': {
        'gpu': '1002:6821-4.0.20-3.2.8',
        'os': 'Mac-10.13.3',
        'pool': 'chrome.tests.perf',
        'synthetic_product_name': 'MacBookPro11,5_x86-64-i7-4870HQ_AMD Radeon R8 M370X 4.0.20 [3.2.8]_Intel Haswell Iris Pro Graphics 5200 4.0.20 [3.2.8]_16384_APPLE SSD SM0512G',
    },
  },
}

# pylint: enable=line-too-long


def update_all_builders(file_path):
  return (_update_builders(BUILDERS, file_path) and
          is_perf_benchmarks_scheduling_valid(file_path, sys.stderr))


def update_all_fyi_builders(file_path):
  return _update_builders(FYI_BUILDERS, file_path)


def _update_builders(builders_dict, file_path):
  tests = {}
  tests['AAAAA1 AUTOGENERATED FILE DO NOT EDIT'] = {}
  tests['AAAAA2 See //tools/perf/generate_perf_data to make changes'] = {}

  for name, config in builders_dict.iteritems():
    tests[name] = generate_builder_config(config, name)

  with open(file_path, 'w') as fp:
    json.dump(tests, fp, indent=2, separators=(',', ': '), sort_keys=True)
    fp.write('\n')
  return True


def merge_dicts(*dict_args):
  result = {}
  for dictionary in dict_args:
    result.update(dictionary)
  return result


class BenchmarkMetadata(object):
  def __init__(self, emails, component='', documentation_url='', stories=None):
    """An object to hold information about a benchmark.

    Args:
      emails: A string with a comma separated list of owner emails.
      component: An optional string with a component for filing bugs about this
        benchmark.
      documentation_url: An optional string with a URL where documentation
        about the benchmark can be found.
      stories: An optional list of benchmark_utils.StoryInfo tuples with
        information about stories contained in this benchmark.
    """
    self.emails = emails
    self.component = component
    self.documentation_url = documentation_url
    if stories is not None:
      assert isinstance(stories, list)
      self.stories = stories
    else:
      self.stories = []

  @property
  def tags(self):
    """Return a comma separated list of all tags used by benchmark stories."""
    return ','.join(sorted(set().union(*(s.tags for s in self.stories))))


GTEST_BENCHMARKS = {
    'angle_perftests': BenchmarkMetadata(
        'jmadill@chromium.org, chrome-gpu-perf-owners@chromium.org',
        'Internals>GPU>ANGLE'),
    'base_perftests': BenchmarkMetadata(
        'skyostil@chromium.org, gab@chromium.org',
        'Internals>SequenceManager',
        ('https://chromium.googlesource.com/chromium/src/+/HEAD/base/' +
         'README.md#performance-testing')),
    'validating_command_buffer_perftests': BenchmarkMetadata(
        'piman@chromium.org, chrome-gpu-perf-owners@chromium.org',
        'Internals>GPU'),
    'passthrough_command_buffer_perftests': BenchmarkMetadata(
        'net-dev@chromium.org',
        'Internals>Network'),
    'net_perftests': BenchmarkMetadata(
        'net-dev@chromium.org',
        'Internals>Network'),
    'gpu_perftests': BenchmarkMetadata(
        'reveman@chromium.org, chrome-gpu-perf-owners@chromium.org',
        'Internals>GPU'),
    'tracing_perftests': BenchmarkMetadata(
        'kkraynov@chromium.org, primiano@chromium.org'),
    'load_library_perf_tests': BenchmarkMetadata(
        'xhwang@chromium.org, crouleau@chromium.org',
        'Internals>Media>Encrypted'),
    'performance_browser_tests': BenchmarkMetadata(
        'miu@chromium.org', 'Internals>Media>ScreenCapture'),
    'media_perftests': BenchmarkMetadata(
        'crouleau@chromium.org, dalecurtis@chromium.org',
        'Internals>Media'),
    'views_perftests': BenchmarkMetadata(
        'tapted@chromium.org', 'Internals>Views'),
    'components_perftests': BenchmarkMetadata('csharrison@chromium.org'),
    'dawn_perf_tests': BenchmarkMetadata(
        'enga@chromium.org, chrome-gpu-perf-owners@chromium.org',
        'Internals>GPU>Dawn',
        'https://dawn.googlesource.com/dawn/+/HEAD/src/tests/perf_tests/README.md'),
}


RESOURCE_SIZES_METADATA = BenchmarkMetadata(
    'agrieve@chromium.org, jbudorick@chromium.org',
    'Build',
    ('https://chromium.googlesource.com/chromium/src/+/HEAD/'
     'tools/binary_size/README.md#resource_sizes_py'))


OTHER_BENCHMARKS = {
    'resource_sizes_chrome_apk': RESOURCE_SIZES_METADATA,
    'resource_sizes_chrome_public_apk': RESOURCE_SIZES_METADATA,
    'resource_sizes_chrome_modern_minimal_apks': RESOURCE_SIZES_METADATA,
    'resource_sizes_chrome_modern_public_minimal_apks': RESOURCE_SIZES_METADATA,
    'resource_sizes_monochrome_minimal_apks': RESOURCE_SIZES_METADATA,
    'resource_sizes_monochrome_public_minimal_apks': RESOURCE_SIZES_METADATA,
    'resource_sizes_system_webview_apk': RESOURCE_SIZES_METADATA,
    'resource_sizes_system_webview_google_apk': RESOURCE_SIZES_METADATA,
}


SYSTEM_HEALTH_BENCHMARKS = set([
    'system_health.common_desktop',
    'system_health.common_mobile',
    'system_health.memory_desktop',
    'system_health.memory_mobile',
])


# Valid test suite (benchmark) names should match this regex.
RE_VALID_TEST_SUITE_NAME = r'^[\w._-]+$'


def _get_telemetry_perf_benchmarks_metadata():
  metadata = {}
  for benchmark in benchmark_finders.GetOfficialBenchmarks():
    benchmark_name = benchmark.Name()
    emails = decorators.GetEmails(benchmark)
    if emails:
      emails = ', '.join(emails)
    metadata[benchmark_name] = BenchmarkMetadata(
        emails=emails,
        component=decorators.GetComponent(benchmark),
        documentation_url=decorators.GetDocumentationLink(benchmark),
        stories=benchmark_utils.GetBenchmarkStoryInfo(benchmark()))
  return metadata


TELEMETRY_PERF_BENCHMARKS = _get_telemetry_perf_benchmarks_metadata()


def get_scheduled_non_telemetry_benchmarks(perf_waterfall_file):
  test_names = set()

  with open(perf_waterfall_file) as f:
    tests_by_builder = json.load(f)

  script_tests = []
  for tests in tests_by_builder.values():
    if 'isolated_scripts' in tests:
      script_tests += tests['isolated_scripts']
    if 'scripts' in tests:
      script_tests += tests['scripts']

  for s in script_tests:
    name = s['name']
    # TODO(eyaich): Determine new way to generate ownership based
    # on the benchmark bot map instead of on the generated tests
    # for new perf recipe.
    if not name in ('performance_test_suite',
                    'performance_webview_test_suite',
                    'performance_weblayer_test_suite'):
      test_names.add(name)

  for platform in bot_platforms.ALL_PLATFORMS:
    for executable in platform.executables:
      test_names.add(executable.name)

  return test_names


def is_perf_benchmarks_scheduling_valid(
    perf_waterfall_file, outstream):
  """Validates that all existing benchmarks are properly scheduled.

  Return: True if all benchmarks are properly scheduled, False otherwise.
  """
  scheduled_non_telemetry_tests = get_scheduled_non_telemetry_benchmarks(
      perf_waterfall_file)
  all_perf_gtests = set(GTEST_BENCHMARKS)
  all_perf_other_tests = set(OTHER_BENCHMARKS)

  error_messages = []

  for test_name in all_perf_gtests - scheduled_non_telemetry_tests:
    error_messages.append(
        'Benchmark %s is tracked but not scheduled on any perf waterfall '
        'builders. Either schedule or remove it from GTEST_BENCHMARKS.'
        % test_name)

  for test_name in all_perf_other_tests - scheduled_non_telemetry_tests:
    error_messages.append(
        'Benchmark %s is tracked but not scheduled on any perf waterfall '
        'builders. Either schedule or remove it from OTHER_BENCHMARKS.'
        % test_name)

  for test_name in scheduled_non_telemetry_tests.difference(
      all_perf_gtests, all_perf_other_tests):
    error_messages.append(
        'Benchmark %s is scheduled on perf waterfall but not tracked. Please '
        'add an entry for it in GTEST_BENCHMARKS or OTHER_BENCHMARKS in'
        '//tools/perf/core/perf_data_generator.py.' % test_name)

  for message in error_messages:
    print('*', textwrap.fill(message, 70), '\n', file=outstream)

  return not error_messages


# Verify that all benchmarks have owners except those on the whitelist.
def _verify_benchmark_owners(benchmark_metadatas):
  unowned_benchmarks = set()
  for benchmark_name in benchmark_metadatas:
    if benchmark_metadatas[benchmark_name].emails is None:
      unowned_benchmarks.add(benchmark_name)

  assert not unowned_benchmarks, (
      'All benchmarks must have owners. Please add owners for the following '
      'benchmarks:\n%s' % '\n'.join(unowned_benchmarks))


def update_benchmark_csv(file_path):
  """Updates go/chrome-benchmarks.

  Updates telemetry/perf/benchmark.csv containing the current benchmark names,
  owners, and components. Requires that all benchmarks have owners.
  """
  header_data = [['AUTOGENERATED FILE DO NOT EDIT'],
      ['See https://bit.ly/update-benchmarks-info to make changes'],
      ['Benchmark name', 'Individual owners', 'Component', 'Documentation',
       'Tags']
  ]

  csv_data = []
  benchmark_metadatas = merge_dicts(
      GTEST_BENCHMARKS, OTHER_BENCHMARKS, TELEMETRY_PERF_BENCHMARKS)
  _verify_benchmark_owners(benchmark_metadatas)

  undocumented_benchmarks = set()
  for benchmark_name in benchmark_metadatas:
    if not re.match(RE_VALID_TEST_SUITE_NAME, benchmark_name):
      raise ValueError('Invalid benchmark name: %s' % benchmark_name)
    if not benchmark_metadatas[benchmark_name].documentation_url:
      undocumented_benchmarks.add(benchmark_name)
    csv_data.append([
        benchmark_name,
        benchmark_metadatas[benchmark_name].emails,
        benchmark_metadatas[benchmark_name].component,
        benchmark_metadatas[benchmark_name].documentation_url,
        benchmark_metadatas[benchmark_name].tags,
    ])
  if undocumented_benchmarks != ub_module.UNDOCUMENTED_BENCHMARKS:
    error_message = (
      'The list of known undocumented benchmarks does not reflect the actual '
      'ones.\n')
    if undocumented_benchmarks - ub_module.UNDOCUMENTED_BENCHMARKS:
      error_message += (
          'New undocumented benchmarks found. Please document them before '
          'enabling on perf waterfall: %s' % (
            ','.join(b for b in undocumented_benchmarks -
                     ub_module.UNDOCUMENTED_BENCHMARKS)))
    if ub_module.UNDOCUMENTED_BENCHMARKS - undocumented_benchmarks:
      error_message += (
          'These benchmarks are already documented. Please remove them from '
          'the UNDOCUMENTED_BENCHMARKS list in undocumented_benchmarks.py: %s' %
          (','.join(b for b in ub_module.UNDOCUMENTED_BENCHMARKS -
                    undocumented_benchmarks)))

    raise ValueError(error_message)

  csv_data = sorted(csv_data, key=lambda b: b[0])
  csv_data = header_data + csv_data

  with open(file_path, 'wb') as f:
    writer = csv.writer(f, lineterminator='\n')
    writer.writerows(csv_data)
  return True


def update_system_health_stories(filepath):
  """Updates bit.ly/csh-stories.

  Updates tools/perf/system_health_stories.csv containing the current set
  of system health stories.
  """
  header_data = [['AUTOGENERATED FILE DO NOT EDIT'],
      ['See //tools/perf/core/perf_data_generator.py to make changes'],
      ['Story', 'Description', 'Platforms', 'Tags']
  ]

  stories = {}
  for benchmark_name in sorted(SYSTEM_HEALTH_BENCHMARKS):
    platform = benchmark_name.rsplit('_', 1)[-1]
    for story in TELEMETRY_PERF_BENCHMARKS[benchmark_name].stories:
      if story.name not in stories:
        stories[story.name] = {
            'description': story.description,
            'platforms': set([platform]),
            'tags': set(story.tags)
        }
      else:
        stories[story.name]['platforms'].add(platform)
        stories[story.name]['tags'].update(story.tags)

  with open(filepath, 'wb') as f:
    writer = csv.writer(f, lineterminator='\n')
    for row in header_data:
      writer.writerow(row)
    for story_name, info in sorted(stories.items()):
      platforms = ','.join(sorted(info['platforms']))
      tags = ','.join(sorted(info['tags']))
      writer.writerow([story_name, info['description'], platforms, tags])
  return True


def update_labs_docs_md(filepath):
  configs = collections.defaultdict(list)
  for tester in bot_platforms.ALL_PLATFORMS:
    if not tester.is_fyi:
      configs[tester.platform].append(tester)

  with open(filepath, 'w') as f:
    f.write("""
[comment]: # (AUTOGENERATED FILE DO NOT EDIT)
[comment]: # (See //tools/perf/generate_perf_data to make changes)

# Platforms tested in the Performance Lab

""")
    for platform, testers in sorted(configs.iteritems()):
      f.write('## %s\n\n' % platform.title())
      testers.sort()
      for tester in testers:
        f.write(' * [{0.name}]({0.builder_url}): {0.description}.\n'.format(
            tester))
      f.write('\n')
  return True


def generate_telemetry_args(tester_config, platform):
  # First determine the browser that you need based on the tester
  browser_name = ''
  # For trybot testing we always use the reference build
  if tester_config.get('testing', False):
    browser_name = 'reference'
  elif 'browser' in tester_config:
    browser_name = 'exact'
  elif tester_config['platform'] == 'android':
    browser_name = 'android-chromium'
  elif tester_config['platform'].startswith('android-'):
    browser_name = tester_config['platform']
  elif tester_config['platform'] == 'chromeos':
    browser_name = 'cros-chrome'
  elif (tester_config['platform'] == 'win'
    and tester_config['target_bits'] == 64):
    browser_name = 'release_x64'
  else:
    browser_name ='release'
  test_args = [
    '-v',
    '--browser=%s' % browser_name,
    '--upload-results',
    '--test-shard-map-filename=%s' % platform.shards_map_file_name,
  ]
  if platform.run_reference_build:
    test_args.append('--run-ref-build')
  if 'browser' in tester_config:
    test_args.append('--browser-executable=../../out/Release/%s' %
                     tester_config['browser'])
    if tester_config['platform'].startswith('android'):
      test_args.append('--device=android')
  return test_args


def generate_gtest_args(test_name):
  # --gtest-benchmark-name so the benchmark name is consistent with the test
  # step's name. This is not always the same as the test binary's name (see
  # crbug.com/870692).
  return [
    '--gtest-benchmark-name', test_name,
  ]


def generate_performance_test(tester_config, test, builder_name):
  isolate_name = test['isolate']

  test_name = test.get('name', isolate_name)
  test_type = test.get('type', TEST_TYPES.TELEMETRY)
  assert test_type in TEST_TYPES.ALL

  shards = test.get('num_shards', None)
  test_args = []
  if test_type == TEST_TYPES.TELEMETRY:
    platform = bot_platforms.PLATFORMS_BY_NAME[builder_name]
    test_args += generate_telemetry_args(tester_config, platform)
    assert shards is None
    shards = platform.num_shards
  elif test_type == TEST_TYPES.GTEST:
    test_args += generate_gtest_args(test_name=test_name)
    assert shards
  # Append any additional args specific to an isolate
  test_args += test.get('extra_args', [])

  result = {
    'args': test_args,
    'isolate_name': isolate_name,
    'name': test_name,
    'override_compile_targets': [
      isolate_name
    ]
  }

  # For now we either get shards from the number of devices specified
  # or a test entry needs to specify the num shards if it supports
  # soft device affinity.

  if tester_config.get('perf_trigger', True):
    result['trigger_script'] = {
        'requires_simultaneous_shard_dispatch': True,
        'script': '//testing/trigger_scripts/perf_device_trigger.py',
        'args': [
            '--multiple-dimension-script-verbose',
            'True'
        ],
    }

  result['merge'] = {
      'script': '//tools/perf/process_perf_results.py',
  }

  result['swarming'] = {
    # Always say this is true regardless of whether the tester
    # supports swarming. It doesn't hurt.
    'can_use_on_swarming_builders': True,
    'expiration': 2 * 60 * 60, # 2 hours pending max
    # TODO(crbug.com/865538): once we have plenty of windows hardwares,
    # to shards perf benchmarks on Win builders, reduce this hard timeout limit
    # to ~2 hrs.
    # Note that the builder seems to time out after 7 hours (crbug.com/1036447),
    # so we must timeout the shards within ~6 hours to allow for other
    # overhead. If the overall builder times out then we
    # don't get data even from the passing shards.
    'hard_timeout': int(6 * 60 * 60), # 6 hours timeout for full suite
    'ignore_task_failure': False,
    # 5.5 hour timeout. Note that this is effectively the timeout for a
    # benchmarking subprocess to run since we intentionally do not stream
    # subprocess output to the task stdout.
    # TODO(crbug.com/865538): Reduce this once we can reduce hard_timeout.
    'io_timeout': int(6 * 60 * 60),
    'dimension_sets': [
      tester_config['dimension']
    ],
  }
  if shards:
    result['swarming']['shards'] = shards
  return result


def generate_builder_config(condensed_config, builder_name):
  config = {}

  if 'additional_compile_targets' in condensed_config:
    config['additional_compile_targets'] = (
        condensed_config['additional_compile_targets'])

  condensed_tests = condensed_config.get('tests')
  if condensed_tests:
    gtest_tests = []
    telemetry_tests = []
    other_tests = []
    for test in condensed_tests:
      generated_script = generate_performance_test(
          condensed_config, test, builder_name)
      test_type = test.get('type', TEST_TYPES.TELEMETRY)
      if test_type == TEST_TYPES.GTEST:
        gtest_tests.append(generated_script)
      elif test_type == TEST_TYPES.TELEMETRY:
        telemetry_tests.append(generated_script)
      elif test_type == TEST_TYPES.GENERIC:
        other_tests.append(generated_script)
      else:
        raise ValueError(
            'perf_data_generator.py does not understand test type %s.'
                % test_type)
    gtest_tests.sort(key=lambda x: x['name'])
    telemetry_tests.sort(key=lambda x: x['name'])
    other_tests.sort(key=lambda x: x['name'])

    # Put Telemetry tests as the end since they tend to run longer to avoid
    # starving gtests (see crbug.com/873389).
    config['isolated_scripts'] = gtest_tests + telemetry_tests + other_tests

  return config


# List of all updater functions and the file they generate. The updater
# functions must return True on success and False otherwise. File paths are
# relative to chromium src and should use posix path separators (i.e. '/').
ALL_UPDATERS_AND_FILES = [
  (update_all_builders, 'testing/buildbot/chromium.perf.json'),
  (update_all_fyi_builders, 'testing/buildbot/chromium.perf.fyi.json'),
  (update_benchmark_csv, 'tools/perf/benchmark.csv'),
  (update_system_health_stories, 'tools/perf/system_health_stories.csv'),
  (update_labs_docs_md, 'docs/speed/perf_lab_platforms.md'),
]


def _source_filepath(posix_path):
  return os.path.join(path_util.GetChromiumSrcDir(), *posix_path.split('/'))


def validate_all_files():
  """Validate all generated files."""
  tempdir = tempfile.mkdtemp()
  try:
    for run_updater, src_file in ALL_UPDATERS_AND_FILES:
      real_filepath = _source_filepath(src_file)
      temp_filepath = os.path.join(tempdir, os.path.basename(real_filepath))
      if not (os.path.exists(real_filepath) and
              run_updater(temp_filepath) and
              filecmp.cmp(temp_filepath, real_filepath)):
        return False
  finally:
    shutil.rmtree(tempdir)
  return True


def update_all_files():
  """Update all generated files."""
  for run_updater, src_file in ALL_UPDATERS_AND_FILES:
    if not run_updater(_source_filepath(src_file)):
      print('Failed updating:', src_file)
      return False
    print('Updated:', src_file)
  return True


def main(args):
  parser = argparse.ArgumentParser(
      description=('Generate perf test\' json config and benchmark.csv. '
                   'This needs to be done anytime you add/remove any existing'
                   'benchmarks in tools/perf/benchmarks.'))
  parser.add_argument(
      '--validate-only', action='store_true', default=False,
      help=('Validate whether the perf json generated will be the same as the '
            'existing configs. This does not change the contain of existing '
            'configs'))
  options = parser.parse_args(args)

  if options.validate_only:
    if validate_all_files():
      print('All the perf config files are up-to-date. \\o/')
      return 0
    else:
      print('Not all perf config files are up-to-date. Please run %s '
            'to update them.' % sys.argv[0])
      return 1
  else:
    return 0 if update_all_files() else 1
