# Move

A mouse move action. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a move action, this property is  always set to &#x60;move&#x60;.  | [default to 'move']
**x** | **int** | The x-coordinate to move to.  | 
**y** | **int** | The y-coordinate to move to.  | 

## Example

```python
from openapi_client.models.move import Move

# TODO update the JSON string below
json = "{}"
# create an instance of Move from a JSON string
move_instance = Move.from_json(json)
# print the JSON string representation of the object
print(Move.to_json())

# convert the object into a dict
move_dict = move_instance.to_dict()
# create an instance of Move from a dict
move_from_dict = Move.from_dict(move_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


