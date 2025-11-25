# ResponseItemList

A list of Response items.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of object returned, must be &#x60;list&#x60;. | 
**data** | [**List[ItemResource]**](ItemResource.md) | A list of items used to generate this response. | 
**has_more** | **bool** | Whether there are more items available. | 
**first_id** | **str** | The ID of the first item in the list. | 
**last_id** | **str** | The ID of the last item in the list. | 

## Example

```python
from openapi_client.models.response_item_list import ResponseItemList

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseItemList from a JSON string
response_item_list_instance = ResponseItemList.from_json(json)
# print the JSON string representation of the object
print(ResponseItemList.to_json())

# convert the object into a dict
response_item_list_dict = response_item_list_instance.to_dict()
# create an instance of ResponseItemList from a dict
response_item_list_from_dict = ResponseItemList.from_dict(response_item_list_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


