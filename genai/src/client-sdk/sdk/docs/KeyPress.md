# KeyPress

A collection of keypresses the model would like to perform. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a keypress action, this property is  always set to &#x60;keypress&#x60;.  | [default to 'keypress']
**keys** | **List[str]** | The combination of keys the model is requesting to be pressed. This is an array of strings, each representing a key.  | 

## Example

```python
from openapi_client.models.key_press import KeyPress

# TODO update the JSON string below
json = "{}"
# create an instance of KeyPress from a JSON string
key_press_instance = KeyPress.from_json(json)
# print the JSON string representation of the object
print(KeyPress.to_json())

# convert the object into a dict
key_press_dict = key_press_instance.to_dict()
# create an instance of KeyPress from a dict
key_press_from_dict = KeyPress.from_dict(key_press_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


