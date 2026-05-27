# CreateRunRequestToolChoice


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the tool. If type is &#x60;function&#x60;, the function name must be set | 
**function** | [**AssistantsNamedToolChoiceFunction**](AssistantsNamedToolChoiceFunction.md) |  | [optional] 

## Example

```python
from openapi_client.models.create_run_request_tool_choice import CreateRunRequestToolChoice

# TODO update the JSON string below
json = "{}"
# create an instance of CreateRunRequestToolChoice from a JSON string
create_run_request_tool_choice_instance = CreateRunRequestToolChoice.from_json(json)
# print the JSON string representation of the object
print(CreateRunRequestToolChoice.to_json())

# convert the object into a dict
create_run_request_tool_choice_dict = create_run_request_tool_choice_instance.to_dict()
# create an instance of CreateRunRequestToolChoice from a dict
create_run_request_tool_choice_from_dict = CreateRunRequestToolChoice.from_dict(create_run_request_tool_choice_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


