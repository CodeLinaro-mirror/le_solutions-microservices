# CreateEvalItem

A chat message that makes up the prompt or context. May include variable references to the \"item\" namespace, ie {{item.name}}.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message input. One of &#x60;user&#x60;, &#x60;assistant&#x60;, &#x60;system&#x60;, or &#x60;developer&#x60;.  | 
**content** | [**EvalItemContent**](EvalItemContent.md) |  | 
**type** | **str** | The type of the message input. Always &#x60;message&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.create_eval_item import CreateEvalItem

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalItem from a JSON string
create_eval_item_instance = CreateEvalItem.from_json(json)
# print the JSON string representation of the object
print(CreateEvalItem.to_json())

# convert the object into a dict
create_eval_item_dict = create_eval_item_instance.to_dict()
# create an instance of CreateEvalItem from a dict
create_eval_item_from_dict = CreateEvalItem.from_dict(create_eval_item_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


