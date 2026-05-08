# ResponsePropertiesToolChoice

How the model should select which tool (or tools) to use when generating a response. See the `tools` parameter to see how to specify which tools the model can call. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of hosted tool the model should to use. Learn more about [built-in tools](/docs/guides/tools).  Allowed values are: - &#x60;file_search&#x60; - &#x60;web_search_preview&#x60; - &#x60;computer_use_preview&#x60;  | 
**name** | **str** | The name of the function to call. | 

## Example

```python
from openapi_client.models.response_properties_tool_choice import ResponsePropertiesToolChoice

# TODO update the JSON string below
json = "{}"
# create an instance of ResponsePropertiesToolChoice from a JSON string
response_properties_tool_choice_instance = ResponsePropertiesToolChoice.from_json(json)
# print the JSON string representation of the object
print(ResponsePropertiesToolChoice.to_json())

# convert the object into a dict
response_properties_tool_choice_dict = response_properties_tool_choice_instance.to_dict()
# create an instance of ResponsePropertiesToolChoice from a dict
response_properties_tool_choice_from_dict = ResponsePropertiesToolChoice.from_dict(response_properties_tool_choice_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


