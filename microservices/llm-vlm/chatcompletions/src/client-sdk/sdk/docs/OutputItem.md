# OutputItem


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The unique identifier of the reasoning content.  | 
**type** | **str** | The type of the output message. Always &#x60;message&#x60;.  | 
**role** | **str** | The role of the output message. Always &#x60;assistant&#x60;.  | 
**content** | [**List[OutputContent]**](OutputContent.md) | The content of the output message.  | 
**status** | **str** | The status of the item. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when items are returned via API.  | 
**queries** | **List[str]** | The queries used to search for files.  | 
**results** | [**List[FileSearchToolCallResultsInner]**](FileSearchToolCallResultsInner.md) | The results of the file search tool call.  | [optional] 
**call_id** | **str** | An identifier used when responding to the tool call with output.  | 
**name** | **str** | The name of the function to run.  | 
**arguments** | **str** | A JSON string of the arguments to pass to the function.  | 
**action** | [**ComputerAction**](ComputerAction.md) |  | 
**pending_safety_checks** | [**List[ComputerToolCallSafetyCheck]**](ComputerToolCallSafetyCheck.md) | The pending safety checks for the computer call.  | 
**summary** | [**List[ReasoningItemSummaryInner]**](ReasoningItemSummaryInner.md) | Reasoning text contents.  | 

## Example

```python
from openapi_client.models.output_item import OutputItem

# TODO update the JSON string below
json = "{}"
# create an instance of OutputItem from a JSON string
output_item_instance = OutputItem.from_json(json)
# print the JSON string representation of the object
print(OutputItem.to_json())

# convert the object into a dict
output_item_dict = output_item_instance.to_dict()
# create an instance of OutputItem from a dict
output_item_from_dict = OutputItem.from_dict(output_item_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


