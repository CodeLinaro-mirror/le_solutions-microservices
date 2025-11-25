# Scroll

A scroll action. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a scroll action, this property is  always set to &#x60;scroll&#x60;.  | [default to 'scroll']
**x** | **int** | The x-coordinate where the scroll occurred.  | 
**y** | **int** | The y-coordinate where the scroll occurred.  | 
**scroll_x** | **int** | The horizontal scroll distance.  | 
**scroll_y** | **int** | The vertical scroll distance.  | 

## Example

```python
from openapi_client.models.scroll import Scroll

# TODO update the JSON string below
json = "{}"
# create an instance of Scroll from a JSON string
scroll_instance = Scroll.from_json(json)
# print the JSON string representation of the object
print(Scroll.to_json())

# convert the object into a dict
scroll_dict = scroll_instance.to_dict()
# create an instance of Scroll from a dict
scroll_from_dict = Scroll.from_dict(scroll_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


