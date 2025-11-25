# CreateThreadRequest

Options to create a new thread. If no thread is provided when running a  request, an empty thread will be created. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**messages** | [**List[CreateMessageRequest]**](CreateMessageRequest.md) | A list of [messages](/docs/api-reference/messages) to start the thread with. | [optional] 
**tool_resources** | [**CreateThreadRequestToolResources**](CreateThreadRequestToolResources.md) |  | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 

## Example

```python
from openapi_client.models.create_thread_request import CreateThreadRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateThreadRequest from a JSON string
create_thread_request_instance = CreateThreadRequest.from_json(json)
# print the JSON string representation of the object
print(CreateThreadRequest.to_json())

# convert the object into a dict
create_thread_request_dict = create_thread_request_instance.to_dict()
# create an instance of CreateThreadRequest from a dict
create_thread_request_from_dict = CreateThreadRequest.from_dict(create_thread_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


