# ItemReferenceInputMessages


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of input messages. Always &#x60;item_reference&#x60;. | 
**item_reference** | **str** | A reference to a variable in the \&quot;item\&quot; namespace. Ie, \&quot;item.name\&quot; | 

## Example

```python
from openapi_client.models.item_reference_input_messages import ItemReferenceInputMessages

# TODO update the JSON string below
json = "{}"
# create an instance of ItemReferenceInputMessages from a JSON string
item_reference_input_messages_instance = ItemReferenceInputMessages.from_json(json)
# print the JSON string representation of the object
print(ItemReferenceInputMessages.to_json())

# convert the object into a dict
item_reference_input_messages_dict = item_reference_input_messages_instance.to_dict()
# create an instance of ItemReferenceInputMessages from a dict
item_reference_input_messages_from_dict = ItemReferenceInputMessages.from_dict(item_reference_input_messages_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


