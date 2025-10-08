# ReasoningItem

A description of the chain of thought used by a reasoning model while generating a response. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the object. Always &#x60;reasoning&#x60;.  | 
**id** | **str** | The unique identifier of the reasoning content.  | 
**summary** | [**List[ReasoningItemSummaryInner]**](ReasoningItemSummaryInner.md) | Reasoning text contents.  | 
**status** | **str** | The status of the item. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when items are returned via API.  | [optional] 

## Example

```python
from openapi_client.models.reasoning_item import ReasoningItem

# TODO update the JSON string below
json = "{}"
# create an instance of ReasoningItem from a JSON string
reasoning_item_instance = ReasoningItem.from_json(json)
# print the JSON string representation of the object
print(ReasoningItem.to_json())

# convert the object into a dict
reasoning_item_dict = reasoning_item_instance.to_dict()
# create an instance of ReasoningItem from a dict
reasoning_item_from_dict = ReasoningItem.from_dict(reasoning_item_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


