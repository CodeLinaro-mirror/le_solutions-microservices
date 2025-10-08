# ComputerUsePreviewTool

A tool that controls a virtual computer.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the computer use tool. Always &#x60;computer_use_preview&#x60;. | [default to 'computer_use_preview']
**environment** | **str** | The type of computer environment to control. | 
**display_width** | **int** | The width of the computer display. | 
**display_height** | **int** | The height of the computer display. | 

## Example

```python
from openapi_client.models.computer_use_preview_tool import ComputerUsePreviewTool

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerUsePreviewTool from a JSON string
computer_use_preview_tool_instance = ComputerUsePreviewTool.from_json(json)
# print the JSON string representation of the object
print(ComputerUsePreviewTool.to_json())

# convert the object into a dict
computer_use_preview_tool_dict = computer_use_preview_tool_instance.to_dict()
# create an instance of ComputerUsePreviewTool from a dict
computer_use_preview_tool_from_dict = ComputerUsePreviewTool.from_dict(computer_use_preview_tool_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


