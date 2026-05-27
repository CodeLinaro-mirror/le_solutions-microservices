# ReasoningItemSummaryInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the object. Always &#x60;summary_text&#x60;.  | 
**text** | **str** | A short summary of the reasoning used by the model when generating the response.  | 

## Example

```python
from openapi_client.models.reasoning_item_summary_inner import ReasoningItemSummaryInner

# TODO update the JSON string below
json = "{}"
# create an instance of ReasoningItemSummaryInner from a JSON string
reasoning_item_summary_inner_instance = ReasoningItemSummaryInner.from_json(json)
# print the JSON string representation of the object
print(ReasoningItemSummaryInner.to_json())

# convert the object into a dict
reasoning_item_summary_inner_dict = reasoning_item_summary_inner_instance.to_dict()
# create an instance of ReasoningItemSummaryInner from a dict
reasoning_item_summary_inner_from_dict = ReasoningItemSummaryInner.from_dict(reasoning_item_summary_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


