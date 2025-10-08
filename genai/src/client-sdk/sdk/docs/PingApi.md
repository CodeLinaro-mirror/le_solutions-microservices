# openapi_client.PingApi

All URIs are relative to *http://localhost/apis*

Method | HTTP request | Description
------------- | ------------- | -------------
[**ping**](PingApi.md#ping) | **GET** /v1/ping | Check if server is accessable to the client


# **ping**
> PingResponse ping()

Check if server is accessable to the client

Ping the server to check it's alive and well. Ping will return Pong if you provide the correct BearerToken

### Example


```python
# ----------------------------------------
# Import SDK modules and models
# ----------------------------------------
from openapi_client.api import PingApi
from openapi_client import Configuration, ApiClient

# ----------------------------------------
# Initialize API client with server configuration
# ----------------------------------------
config = Configuration(host="http://localhost:8080")
client = ApiClient(configuration=config)

# ----------------------------------------
# Ping Api Instance
# ----------------------------------------
ping_api = PingApi(api_client=client)

# Call different variations of the ping endpoint
ping_response = ping_api.ping()

# Print responses
print(f'ping response = {ping_response}')
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**PingResponse**](PingResponse.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | Success |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

