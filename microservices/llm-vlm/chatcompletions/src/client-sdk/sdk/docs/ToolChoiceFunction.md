# ToolChoiceFunction

Use this option to force the model to call a specific function. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | For function calling, the type is always &#x60;function&#x60;. | 
**name** | **str** | The name of the function to call. | 

## Example

```python
from openapi_client.models.tool_choice_function import ToolChoiceFunction

# TODO update the JSON string below
json = "{}"
# create an instance of ToolChoiceFunction from a JSON string
tool_choice_function_instance = ToolChoiceFunction.from_json(json)
# print the JSON string representation of the object
print(ToolChoiceFunction.to_json())

# convert the object into a dict
tool_choice_function_dict = tool_choice_function_instance.to_dict()
# create an instance of ToolChoiceFunction from a dict
tool_choice_function_from_dict = ToolChoiceFunction.from_dict(tool_choice_function_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


