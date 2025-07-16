# InputItem


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message input. One of &#x60;user&#x60;, &#x60;assistant&#x60;, &#x60;system&#x60;, or &#x60;developer&#x60;.  | 
**content** | [**EasyInputMessageContent**](EasyInputMessageContent.md) |  | 
**type** | **str** | The type of the message input. Always &#x60;message&#x60;.  | [optional] 
**id** | **str** | The ID of the item to reference. | 

## Example

```python
from openapi_client.models.input_item import InputItem

# TODO update the JSON string below
json = "{}"
# create an instance of InputItem from a JSON string
input_item_instance = InputItem.from_json(json)
# print the JSON string representation of the object
print(InputItem.to_json())

# convert the object into a dict
input_item_dict = input_item_instance.to_dict()
# create an instance of InputItem from a dict
input_item_from_dict = InputItem.from_dict(input_item_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


