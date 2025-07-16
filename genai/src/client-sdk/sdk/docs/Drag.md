# Drag

A drag action. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a drag action, this property is  always set to &#x60;drag&#x60;.  | [default to 'drag']
**path** | [**List[Coordinate]**](Coordinate.md) | An array of coordinates representing the path of the drag action. Coordinates will appear as an array of objects, eg &#x60;&#x60;&#x60; [   { x: 100, y: 200 },   { x: 200, y: 300 } ] &#x60;&#x60;&#x60;  | 

## Example

```python
from openapi_client.models.drag import Drag

# TODO update the JSON string below
json = "{}"
# create an instance of Drag from a JSON string
drag_instance = Drag.from_json(json)
# print the JSON string representation of the object
print(Drag.to_json())

# convert the object into a dict
drag_dict = drag_instance.to_dict()
# create an instance of Drag from a dict
drag_from_dict = Drag.from_dict(drag_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


