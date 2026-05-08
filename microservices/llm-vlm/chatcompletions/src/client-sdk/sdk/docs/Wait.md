# Wait

A wait action. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a wait action, this property is  always set to &#x60;wait&#x60;.  | [default to 'wait']

## Example

```python
from openapi_client.models.wait import Wait

# TODO update the JSON string below
json = "{}"
# create an instance of Wait from a JSON string
wait_instance = Wait.from_json(json)
# print the JSON string representation of the object
print(Wait.to_json())

# convert the object into a dict
wait_dict = wait_instance.to_dict()
# create an instance of Wait from a dict
wait_from_dict = Wait.from_dict(wait_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


