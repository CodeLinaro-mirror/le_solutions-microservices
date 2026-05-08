# openapi_client.CompletionsApi

All URIs are relative to *http://localhost/apis*

Method | HTTP request | Description
------------- | ------------- | -------------
[**create_completion**](CompletionsApi.md#create_completion) | **POST** /v1/completions | Creates a completion for the provided prompt and parameters.


# **create_completion**
> CreateCompletionResponse create_completion(create_completion_request)

Creates a completion for the provided prompt and parameters.

### Example


```python
# ----------------------------------------
# Import SDK modules and models
# ----------------------------------------
from openapi_client.api import CompletionsApi
from openapi_client import Configuration, ApiClient
from openapi_client.models.create_completion_request import CreateCompletionRequest
from openapi_client.models.create_completion_request_prompt import CreateCompletionRequestPrompt

# ----------------------------------------
# Initialize API client with server configuration
# ----------------------------------------
config = Configuration(host="http://localhost:8080")
client = ApiClient(configuration=config)

# ----------------------------------------
# Completion API: Generate text from a prompt
# ----------------------------------------
completion_api = CompletionsApi(api_client=client)

# Create a prompt object (can be a string or list of strings/tokens)
prompt_obj = CreateCompletionRequestPrompt(actual_instance="What is the capital of India")

# Create the completion request with the prompt
create_completion_request = CreateCompletionRequest(prompt=prompt_obj)

# Call the completion API
completion_response = completion_api.create_completion(create_completion_request)
print(f'completion response = {completion_response}')
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **create_completion_request** | [**CreateCompletionRequest**](CreateCompletionRequest.md)|  | 

### Return type

[**CreateCompletionResponse**](CreateCompletionResponse.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | OK |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

