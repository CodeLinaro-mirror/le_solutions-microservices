# openapi_client.HealthApi

All URIs are relative to *http://localhost/apis*

Method | HTTP request | Description
------------- | ------------- | -------------
[**healthcheck**](HealthApi.md#healthcheck) | **GET** /v1/health | Check the health of microservice


# **healthcheck**
> List[UsageRead] healthcheck()

Check the health of microservice

Health Check of various dependencies

### Example


```python
# ----------------------------------------
# Import SDK modules and models
# ----------------------------------------
from openapi_client.api import HealthApi
from openapi_client import Configuration, ApiClient

# ----------------------------------------
# Initialize API client with server configuration
# ----------------------------------------
config = Configuration(host="http://localhost:8080")
client = ApiClient(configuration=config)

# Health Api instance
health_api = HealthApi(api_client=client)

# Call the health check endpoint
health_response = health_api.healthcheck()

print(f'health response = {health_response}')
```



### Parameters

This endpoint does not need any parameter.

### Return type

[**List[UsageRead]**](UsageRead.md)

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

