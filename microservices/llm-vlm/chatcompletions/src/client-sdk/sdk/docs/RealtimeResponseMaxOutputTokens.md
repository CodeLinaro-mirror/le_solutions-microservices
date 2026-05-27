# RealtimeResponseMaxOutputTokens

Maximum number of output tokens for a single assistant response, inclusive of tool calls, that was used in this response. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.realtime_response_max_output_tokens import RealtimeResponseMaxOutputTokens

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeResponseMaxOutputTokens from a JSON string
realtime_response_max_output_tokens_instance = RealtimeResponseMaxOutputTokens.from_json(json)
# print the JSON string representation of the object
print(RealtimeResponseMaxOutputTokens.to_json())

# convert the object into a dict
realtime_response_max_output_tokens_dict = realtime_response_max_output_tokens_instance.to_dict()
# create an instance of RealtimeResponseMaxOutputTokens from a dict
realtime_response_max_output_tokens_from_dict = RealtimeResponseMaxOutputTokens.from_dict(realtime_response_max_output_tokens_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


