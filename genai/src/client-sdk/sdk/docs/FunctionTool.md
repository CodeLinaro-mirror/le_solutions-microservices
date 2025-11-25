# FunctionTool

Defines a function in your own code the model can choose to call.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the function tool. Always &#x60;function&#x60;. | [default to 'function']
**name** | **str** | The name of the function to call. | 
**description** | **str** | A description of the function. Used by the model to determine whether or not to call the function. | [optional] 
**parameters** | **Dict[str, object]** | A JSON schema object describing the parameters of the function. | 
**strict** | **bool** | Whether to enforce strict parameter validation. Default &#x60;true&#x60;. | 

## Example

```python
from openapi_client.models.function_tool import FunctionTool

# TODO update the JSON string below
json = "{}"
# create an instance of FunctionTool from a JSON string
function_tool_instance = FunctionTool.from_json(json)
# print the JSON string representation of the object
print(FunctionTool.to_json())

# convert the object into a dict
function_tool_dict = function_tool_instance.to_dict()
# create an instance of FunctionTool from a dict
function_tool_from_dict = FunctionTool.from_dict(function_tool_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


