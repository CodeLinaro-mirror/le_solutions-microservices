# openapi_client.ChatApi

All URIs are relative to *http://localhost/apis*

Method | HTTP request | Description
------------- | ------------- | -------------
[**add_chat_completion**](ChatApi.md#add_chat_completion) | **POST** /v1/chat/completions/{completion_id} | Adds a chat to existing conversation. 
[**create_chat_completion**](ChatApi.md#create_chat_completion) | **POST** /v1/chat/completions | Creates a chat conversation. 
[**delete_chat_completion**](ChatApi.md#delete_chat_completion) | **DELETE** /v1/chat/completions/{completion_id} | Delete a stored chat completion. 


# **add_chat_completion**
> CreateChatCompletionResponse add_chat_completion(completion_id, create_chat_completion_request)

Adds a chat to existing conversation. 

### Example


```python
# ----------------------------------------
# Import SDK modules and models
# ----------------------------------------
from openapi_client.api import ChatApi
from openapi_client import Configuration, ApiClient
from openapi_client.models.chat_completion_request_message import ChatCompletionRequestMessage
from openapi_client.models.chat_completion_request_user_message import ChatCompletionRequestUserMessage
from openapi_client.models.chat_completion_request_user_message_content import ChatCompletionRequestUserMessageContent
from openapi_client.models.create_chat_completion_request import CreateChatCompletionRequest

# ----------------------------------------
# Initialize API client with server configuration
# ----------------------------------------
config = Configuration(host="http://localhost:8080")
client = ApiClient(configuration=config)


# ----------------------------------------
# Chat API: Create and extend a chat conversation
# ----------------------------------------
chat_api = ChatApi(api_client=client)

# Step 1: Create a chat completion request with a user message
content = ChatCompletionRequestUserMessageContent(actual_instance="What is the capital of India.")
user_msg = ChatCompletionRequestUserMessage(role="user", content=content)
chat_msg = ChatCompletionRequestMessage(actual_instance=user_msg)
request = CreateChatCompletionRequest(messages=[chat_msg])

# Call the chat completion API
response = chat_api.create_chat_completion(create_chat_completion_request=request)
print(f'create chat completion response = {response}')

# Step 2: Add another message to the existing chat using the returned chat ID
content = ChatCompletionRequestUserMessageContent(actual_instance="What is the population of New Delhi.")
user_msg = ChatCompletionRequestUserMessage(role="user", content=content)
chat_msg = ChatCompletionRequestMessage(actual_instance=user_msg)
request = CreateChatCompletionRequest(messages=[chat_msg])

# Add the new message to the existing chat session
response = chat_api.add_chat_completion(completion_id=response.id, create_chat_completion_request=request)
print(f'Add chat completion response = {response}')

```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **completion_id** | **str**| id of conversation | 
 **create_chat_completion_request** | [**CreateChatCompletionRequest**](CreateChatCompletionRequest.md)|  | 

### Return type

[**CreateChatCompletionResponse**](CreateChatCompletionResponse.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json, text/event-stream

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | OK |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **create_chat_completion**
> CreateChatCompletionResponse create_chat_completion(create_chat_completion_request)

Creates a chat conversation. 

### Example


```python
# ----------------------------------------
# Import SDK modules and models
# ----------------------------------------
from openapi_client.api import ChatApi
from openapi_client import Configuration, ApiClient
from openapi_client.models.chat_completion_request_message import ChatCompletionRequestMessage
from openapi_client.models.chat_completion_request_user_message import ChatCompletionRequestUserMessage
from openapi_client.models.chat_completion_request_user_message_content import ChatCompletionRequestUserMessageContent
from openapi_client.models.create_chat_completion_request import CreateChatCompletionRequest

# ----------------------------------------
# Initialize API client with server configuration
# ----------------------------------------
config = Configuration(host="http://localhost:8080")
client = ApiClient(configuration=config)


# ----------------------------------------
# Chat API: Create a chat conversation
# ----------------------------------------
chat_api = ChatApi(api_client=client)

# Step 1: Create a chat completion request with a user message
content = ChatCompletionRequestUserMessageContent(actual_instance="What is the capital of India.")
user_msg = ChatCompletionRequestUserMessage(role="user", content=content)
chat_msg = ChatCompletionRequestMessage(actual_instance=user_msg)
request = CreateChatCompletionRequest(messages=[chat_msg])

# Call the chat completion API
response = chat_api.create_chat_completion(create_chat_completion_request=request)
print(f'create chat completion response = {response}')
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **create_chat_completion_request** | [**CreateChatCompletionRequest**](CreateChatCompletionRequest.md)|  | 

### Return type

[**CreateChatCompletionResponse**](CreateChatCompletionResponse.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json, text/event-stream

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | OK |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **delete_chat_completion**
> ChatCompletionDeleted delete_chat_completion(completion_id)

Delete a stored chat completion. 

### Example


```python
# ----------------------------------------
# Import SDK modules and models
# ----------------------------------------
from openapi_client.api import ChatApi
from openapi_client import Configuration, ApiClient

# ----------------------------------------
# Initialize API client with server configuration
# ----------------------------------------
config = Configuration(host="http://localhost:8080")
client = ApiClient(configuration=config)

chat_api = ChatApi(api_client=client)

# Delete chat completion id
response = chat_api.delete_chat_completion(completion_id='completion_id_example')
print(f'Delete chat completion response = {response}')
```



### Parameters


Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
 **completion_id** | **str**| The ID of the chat completion to delete. | 

### Return type

[**ChatCompletionDeleted**](ChatCompletionDeleted.md)

### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: application/json

### HTTP response details

| Status code | Description | Response headers |
|-------------|-------------|------------------|
**200** | The chat completion was deleted successfully. |  -  |

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

