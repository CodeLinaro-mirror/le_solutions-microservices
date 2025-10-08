# ResponseAllOfIncompleteDetails

Details about why the response is incomplete. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**reason** | **str** | The reason why the response is incomplete. | [optional] 

## Example

```python
from openapi_client.models.response_all_of_incomplete_details import ResponseAllOfIncompleteDetails

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseAllOfIncompleteDetails from a JSON string
response_all_of_incomplete_details_instance = ResponseAllOfIncompleteDetails.from_json(json)
# print the JSON string representation of the object
print(ResponseAllOfIncompleteDetails.to_json())

# convert the object into a dict
response_all_of_incomplete_details_dict = response_all_of_incomplete_details_instance.to_dict()
# create an instance of ResponseAllOfIncompleteDetails from a dict
response_all_of_incomplete_details_from_dict = ResponseAllOfIncompleteDetails.from_dict(response_all_of_incomplete_details_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


