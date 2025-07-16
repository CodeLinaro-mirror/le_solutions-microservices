# ItemReferenceParam

An internal identifier for an item to reference.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of item to reference. Always &#x60;item_reference&#x60;. | [optional] [default to 'item_reference']
**id** | **str** | The ID of the item to reference. | 

## Example

```python
from openapi_client.models.item_reference_param import ItemReferenceParam

# TODO update the JSON string below
json = "{}"
# create an instance of ItemReferenceParam from a JSON string
item_reference_param_instance = ItemReferenceParam.from_json(json)
# print the JSON string representation of the object
print(ItemReferenceParam.to_json())

# convert the object into a dict
item_reference_param_dict = item_reference_param_instance.to_dict()
# create an instance of ItemReferenceParam from a dict
item_reference_param_from_dict = ItemReferenceParam.from_dict(item_reference_param_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


