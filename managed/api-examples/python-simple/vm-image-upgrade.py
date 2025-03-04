#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright (c) YugaByte, Inc.

import json
import requests
from pprint import pprint

# ==================================================================================================
# --- Constants
# ==================================================================================================

# Base url endpoint to make requests to platform. Consists of "platform address + /api/v1"
BASE_URL = "http://localhost:9000/api/v1"

# To identify the customer UUID for this API call, see the "Get Session Info" section at
# https://github.com/yugabyte/yugabyte-db/blob/master/managed/api-examples/python-simple/
#   create-universe.ipynb
CUSTOMER_UUID = "f33e3c9b-75ab-4c30-80ad-cba85646ea39"

# To identify the universe uuid for a given universe, use the the example at
# https://github.com/yugabyte/yugabyte-db/blob/master/managed/api-examples/python-simple/
#   list-universes.ipynb
# to list all universes and filter by name
UNIVERSE_UUID = "4d419e8d-51d0-4c1c-9446-40849d3cec9c"

# Platform api key to be set
X_AUTH_YW_API_TOKEN = "5e8d9e2e-894d-405d-8731-fdc2111e8d57"

DEFAULT_HEADERS = {"Content-Type": "application/json", "X-AUTH-YW-API-TOKEN": X_AUTH_YW_API_TOKEN}

POST_VM_IMAGE_UPGRADE_URL = BASE_URL + \
    "/customers/{customer_uuid}/universes/{universe_uuid}/upgrade/vm"


# ==================================================================================================
# --- Api request
# ==================================================================================================


task_params = {
    "universeUUID": UNIVERSE_UUID,
    "sleepAfterMasterRestartMillis": 0,
    "sleepAfterTServerRestartMillis": 0,
    "machineImages": {
        "e83d5748-82ee-4e06-af3d-b5dbbaa0e0bb": "ami-b63ae0ce"
    },
    "forceAll": "true"
}

pprint(task_params)
print("-----\n\n\n")

# This response includes a task UUID that represents an asynchronous operation. In order to wait
# for this operation to complete
response = requests.post(POST_VM_IMAGE_UPGRADE_URL
                         .format(universe_uuid=UNIVERSE_UUID, customer_uuid=CUSTOMER_UUID),
                         headers=DEFAULT_HEADERS, data=json.dumps(task_params, indent=3))
response_json = response.json()
print(response_json)
