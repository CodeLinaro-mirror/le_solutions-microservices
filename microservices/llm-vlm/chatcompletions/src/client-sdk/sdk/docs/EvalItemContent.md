# EvalItemContent

Text inputs to the model - can contain template strings. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_text&#x60;. | [default to 'input_text']
**text** | **str** | The text output from the model.  | 

## Example

```python
from openapi_client.models.eval_item_content import EvalItemContent

# TODO update the JSON string below
json = "{}"
# create an instance of EvalItemContent from a JSON string
eval_item_content_instance = EvalItemContent.from_json(json)
# print the JSON string representation of the object
print(EvalItemContent.to_json())

# convert the object into a dict
eval_item_content_dict = eval_item_content_instance.to_dict()
# create an instance of EvalItemContent from a dict
eval_item_content_from_dict = EvalItemContent.from_dict(eval_item_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


