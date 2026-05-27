# ToolChoiceTypes

Indicates that the model should use a built-in tool to generate a response. [Learn more about built-in tools](/docs/guides/tools). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of hosted tool the model should to use. Learn more about [built-in tools](/docs/guides/tools).  Allowed values are: - &#x60;file_search&#x60; - &#x60;web_search_preview&#x60; - &#x60;computer_use_preview&#x60;  | 

## Example

```python
from openapi_client.models.tool_choice_types import ToolChoiceTypes

# TODO update the JSON string below
json = "{}"
# create an instance of ToolChoiceTypes from a JSON string
tool_choice_types_instance = ToolChoiceTypes.from_json(json)
# print the JSON string representation of the object
print(ToolChoiceTypes.to_json())

# convert the object into a dict
tool_choice_types_dict = tool_choice_types_instance.to_dict()
# create an instance of ToolChoiceTypes from a dict
tool_choice_types_from_dict = ToolChoiceTypes.from_dict(tool_choice_types_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


