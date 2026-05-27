# DoubleClick

A double click action. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a double click action, this property is  always set to &#x60;double_click&#x60;.  | [default to 'double_click']
**x** | **int** | The x-coordinate where the double click occurred.  | 
**y** | **int** | The y-coordinate where the double click occurred.  | 

## Example

```python
from openapi_client.models.double_click import DoubleClick

# TODO update the JSON string below
json = "{}"
# create an instance of DoubleClick from a JSON string
double_click_instance = DoubleClick.from_json(json)
# print the JSON string representation of the object
print(DoubleClick.to_json())

# convert the object into a dict
double_click_dict = double_click_instance.to_dict()
# create an instance of DoubleClick from a dict
double_click_from_dict = DoubleClick.from_dict(double_click_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


