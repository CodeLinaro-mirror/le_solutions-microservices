# Type

An action to type in text. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a type action, this property is  always set to &#x60;type&#x60;.  | [default to 'type']
**text** | **str** | The text to type.  | 

## Example

```python
from openapi_client.models.type import Type

# TODO update the JSON string below
json = "{}"
# create an instance of Type from a JSON string
type_instance = Type.from_json(json)
# print the JSON string representation of the object
print(Type.to_json())

# convert the object into a dict
type_dict = type_instance.to_dict()
# create an instance of Type from a dict
type_from_dict = Type.from_dict(type_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


